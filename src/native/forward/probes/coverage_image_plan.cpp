#include "forward/probes/coverage_image_plan.h"
#include "forward/probes/priority_order.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace remix::forward::probes {
  namespace {
    void validateHeader(const Header &header) {
      const auto &p = header.parameters;
      if (p[0] > maxProbes || (p[0] && (p[1] < 8 || p[1] > 512 || !std::has_single_bit(p[1]) ||
          p[2] != uint32_t(std::bit_width(p[1])) || p[3] != levelOffset(p[1], p[2]))))
        throw std::invalid_argument("Invalid coverage image dimensions");
    }
    bool sameGeometry(const Record &a, const Record &b) {
      for (uint32_t axis = 0; axis < 3; ++axis)
        for (const auto pair : {std::pair{a.minimum[axis], b.minimum[axis]}, std::pair{a.maximum[axis], b.maximum[axis]},
             std::pair{a.influenceMinimum[axis], b.influenceMinimum[axis]},
             std::pair{a.influenceMaximum[axis], b.influenceMaximum[axis]}, std::pair{a.position[axis], b.position[axis]}})
          if (std::bit_cast<uint32_t>(pair.first) != std::bit_cast<uint32_t>(pair.second)) return false;
      return std::bit_cast<std::array<uint32_t, 12>>(a.worldToLocal) ==
             std::bit_cast<std::array<uint32_t, 12>>(b.worldToLocal);
    }
  }
  CoverageImagePlan buildCoverageImagePlan(const scene::BakeScene &source, const CoverageBaselineSet &baseline,
      const ReceiverCoveragePlan &plan, const Header &published, const LayerIdentities &identities,
      const CoverageReadyImage &local) {
    validateHeader(published);
    if (!matchesCoverageContext(plan.context, source, baseline) || published.parameters != baseline.parameters ||
        baseline.volumes.size() != published.parameters[0])
      throw std::invalid_argument("Stale coverage image baseline");
    CoverageImagePlan result;
    result.header = published;
    result.context = plan.context;
    std::set<uint64_t> authoredIds;
    float minimumAuthored = std::numeric_limits<float>::infinity(), automaticPriority = 0;
    bool hasAutomatic = false;
    for (uint32_t i = 0; i < published.parameters[0]; ++i) {
      const auto &volume = baseline.volumes[i];
      if (std::bit_cast<std::array<uint32_t, 32>>(volume.record) !=
          std::bit_cast<std::array<uint32_t, 32>>(published.probes[i]))
        throw std::invalid_argument("Published coverage metadata changed");
      const bool authored = volume.protectedCapture || (baseline.composed && !volume.source.automatic);
      if (authored) {
        if (!identities[i] || !authoredIds.insert(identities[i]).second)
          throw std::invalid_argument("Invalid authored coverage identity");
        minimumAuthored = std::min(minimumAuthored, volume.record.minimum[3]);
      } else {
        if (identities[i]) throw std::invalid_argument("Automatic coverage row has an authored identity");
        automaticPriority = hasAutomatic ? std::min(automaticPriority, volume.record.minimum[3]) : volume.record.minimum[3];
        hasAutomatic = true;
      }
      result.identities[i] = identities[i];
      result.copies.push_back({CoverageImageSource::Baseline, i});
    }
    if (plan.replacements.empty()) return result;
    if (baseline.disabled || !baseline.automaticAllowed || plan.disabledBaseline ||
        !std::isfinite(plan.maximumError) || plan.maximumError < 0 ||
        plan.replacements.size() > plan.maximumChanges || plan.maximumChanges > maxProbes ||
        plan.candidateRecords.size() != plan.owners.size() || plan.errors.size() != plan.owners.size() ||
        !matchesCoverageContext(local.context, source, baseline))
      throw std::invalid_argument("Coverage image update is disabled or stale");
    validateHeader(local.header);
    for (uint32_t c = 1; c < 4; ++c)
      if (published.parameters[0] && published.parameters[c] != local.header.parameters[c])
        throw std::invalid_argument("Coverage image source dimensions differ");
    if (!published.parameters[0]) result.header.parameters = {0, local.header.parameters[1], local.header.parameters[2], local.header.parameters[3]};
    std::set<uint32_t> mappedCandidates;
    if (local.layers.size() > maxProbes) throw std::invalid_argument("Too many prepared coverage layers");
    for (const auto &entry : local.layers)
      if (entry.candidate >= plan.candidateRecords.size() || entry.layer >= local.header.parameters[0] ||
          !mappedCandidates.insert(entry.candidate).second)
        throw std::invalid_argument("Invalid prepared coverage mapping");
    std::set<uint32_t> candidates, replaced, localLayers;
    for (const auto &replacement : plan.replacements) {
      const auto candidate = replacement.candidate;
      if (candidate >= plan.candidateRecords.size() || !candidates.insert(candidate).second ||
          !std::isfinite(plan.errors[candidate]) || plan.errors[candidate] < 0 || plan.errors[candidate] > plan.maximumError ||
          !std::isfinite(replacement.gainedArea) || replacement.gainedArea <= 0)
        throw std::invalid_argument("Invalid coverage image candidate");
      const auto found = std::find_if(local.layers.begin(), local.layers.end(), [&](const auto &entry) { return entry.candidate == candidate; });
      if (found == local.layers.end() || found->layer >= local.header.parameters[0] || !localLayers.insert(found->layer).second)
        throw std::invalid_argument("Missing or aliased coverage image layer");
      auto record = local.header.probes[found->layer];
      if (!sameGeometry(record, plan.candidateRecords[candidate]))
        throw std::invalid_argument("Prepared coverage geometry differs from the sampled plan");
      for (float value : std::bit_cast<std::array<float, 32>>(record))
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite prepared coverage metadata");
      uint32_t slot = replacement.baseline;
      float priority = hasAutomatic ? automaticPriority : (std::isfinite(minimumAuthored) ? minimumAuthored - 1 : 0);
      if (slot == UINT32_MAX) {
        slot = result.header.parameters[0];
        if (slot == maxProbes) throw std::invalid_argument("Coverage image update exceeds capacity");
        ++result.header.parameters[0];
        result.copies.push_back({CoverageImageSource::Local, found->layer});
      } else {
        if (slot >= baseline.volumes.size() || !replaced.insert(slot).second || baseline.volumes[slot].protectedCapture ||
            (baseline.composed && !baseline.volumes[slot].source.automatic))
          throw std::invalid_argument("Coverage image update removes a protected or repeated row");
        priority = published.probes[slot].minimum[3];
        result.copies[slot] = {CoverageImageSource::Local, found->layer};
      }
      if (std::isfinite(minimumAuthored)) priority = std::min(priority, minimumAuthored - 1);
      if (!std::isfinite(priority) || (std::isfinite(minimumAuthored) && priority >= minimumAuthored))
        throw std::invalid_argument("Coverage image priority cannot preserve authored precedence");
      record.minimum[3] = priority;
      result.header.probes[slot] = record;
      result.identities[slot] = 0;
    }
    const auto order = priorityOrder(result.header);
    const auto unsorted = result;
    for (uint32_t i = 0; i < result.header.parameters[0]; ++i) {
      result.header.probes[i] = unsorted.header.probes[order[i]];
      result.copies[i] = unsorted.copies[order[i]];
      result.identities[i] = unsorted.identities[order[i]];
    }
    return result;
  }
}
