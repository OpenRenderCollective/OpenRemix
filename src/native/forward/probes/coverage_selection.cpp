#include "forward/probes/coverage_selection.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace remix::forward::probes {
  std::vector<CoverageReplacement> planCoverageReplacements(std::span<const float> areas,
      std::span<const CoverageBaseline> baseline, std::span<const CoverageCandidate> candidates,
      float maximumError, uint32_t capacity, uint32_t maximumChanges, std::stop_token cancelled) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Reflection coverage planning superseded");
    };
    check();
    if (areas.size() > 8192 || capacity > 64 || baseline.size() > capacity || candidates.size() > 192 ||
        maximumChanges > 64 || !std::isfinite(maximumError) || maximumError < 0)
      throw std::invalid_argument("Invalid reflection coverage planning budget");
    for (const float area : areas)
      if (!std::isfinite(area) || area <= 0) throw std::invalid_argument("Invalid receiver area");
    auto validate = [&](const auto &options) {
      for (const auto &option : options) {
        check();
        if (option.receivers.size() > areas.size()) throw std::invalid_argument("Invalid receiver footprint size");
        std::vector<uint8_t> seen(areas.size());
        for (const auto receiver : option.receivers) {
          if (receiver >= areas.size() || seen[receiver]) throw std::invalid_argument("Invalid or duplicate footprint receiver");
          seen[receiver] = true;
        }
      }
    };
    validate(baseline);
    validate(candidates);
    for (const auto &candidate : candidates)
      if (!std::isfinite(candidate.error) || candidate.error < 0)
        throw std::invalid_argument("Invalid candidate projection error");
    std::vector<uint16_t> covered(areas.size());
    std::vector<double> footprintAreas(baseline.size());
    for (size_t b = 0; b < baseline.size(); ++b)
      for (const auto receiver : baseline[b].receivers) {
        ++covered[receiver];
        footprintAreas[b] += areas[receiver];
      }
    std::vector<uint8_t> removed(baseline.size()), selected(candidates.size()), inside(areas.size());
    uint32_t used = uint32_t(baseline.size());
    std::vector<CoverageReplacement> result;
    while (result.size() < maximumChanges) {
      check();
      // Unique ownership changes after each replacement, but total unique
      // entries across all baselines never exceeds the receiver budget.
      std::vector<std::vector<uint32_t>> unique(baseline.size());
      if (used == capacity)
        for (size_t b = 0; b < baseline.size(); ++b)
          if (!removed[b] && !baseline[b].protectedCapture)
            for (const auto receiver : baseline[b].receivers)
              if (covered[receiver] == 1) unique[b].push_back(receiver);
      CoverageReplacement best{UINT32_MAX};
      for (uint32_t c = 0; c < candidates.size(); ++c) {
        check();
        const auto &candidate = candidates[c];
        if (selected[c] || candidate.error > maximumError) continue;
        double gain = 0;
        std::fill(inside.begin(), inside.end(), 0);
        for (const auto receiver : candidate.receivers) {
          inside[receiver] = true;
          if (!covered[receiver]) gain += areas[receiver];
        }
        if (gain <= 0) continue;
        uint32_t victim = UINT32_MAX;
        if (used == capacity) {
          double smallest = INFINITY;
          for (uint32_t b = 0; b < baseline.size(); ++b) {
            if (removed[b] || baseline[b].protectedCapture) continue;
            const bool losesCoverage = std::any_of(unique[b].begin(), unique[b].end(),
                                                   [&](uint32_t receiver) { return !inside[receiver]; });
            if (!losesCoverage && footprintAreas[b] < smallest) { victim = b; smallest = footprintAreas[b]; }
          }
          if (victim == UINT32_MAX) continue;
        }
        if (best.candidate == UINT32_MAX || gain > best.gainedArea ||
            (gain == best.gainedArea && candidate.error < candidates[best.candidate].error))
          best = {c, victim, gain};
      }
      if (best.candidate == UINT32_MAX) break;
      if (best.baseline != UINT32_MAX) {
        removed[best.baseline] = true;
        for (const auto receiver : baseline[best.baseline].receivers) --covered[receiver];
      } else ++used;
      selected[best.candidate] = true;
      for (const auto receiver : candidates[best.candidate].receivers) ++covered[receiver];
      result.push_back(best);
    }
    return result;
  }
}
