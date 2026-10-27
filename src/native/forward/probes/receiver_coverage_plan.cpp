#include "forward/probes/receiver_coverage_plan.h"
#include "forward/probes/coverage_guards.h"
#include <limits>
#include <cmath>
#include <stdexcept>

namespace remix::forward::probes {
  namespace {
    bool contains(const Bounds &bounds, Vec3 position) {
      for (uint32_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(position[axis]) || position[axis] < bounds.minimum[axis] || position[axis] > bounds.maximum[axis]) return false;
      return true;
    }
  }
  ReceiverCoveragePlan planReceiverCoverage(const scene::BakeScene &source,
      std::span<const Capture> baseline, const ReceiverFits &fits, const ReceiverAudit &coverage,
      std::stop_token cancelled) {
    return planReceiverCoverage(source, automaticCoverageBaseline(baseline), fits, coverage, cancelled);
  }
  ReceiverCoverageEvidence retainReceiverCoverage(const scene::BakeScene &source,
      const ReceiverFits &fits, const ReceiverAudit &coverage) {
    if (!coverage.complete() || fits.attempts.size() > 32 || coverage.pairs().size() > 32768)
      throw std::invalid_argument("Cannot retain pending or oversized receiver coverage");
    ReceiverCoverageEvidence result;
    result.sourceContext = coveragePlanContext(source, CoverageBaselineSet{});
    for (const auto &attempt : fits.attempts)
      if (attempt.variants.size() > ReceiverSiteFit::maximumVariants)
        throw std::invalid_argument("Retained receiver variants exceed capacity");
    result.fits = fits;
    result.observations.reserve(coverage.pairs().size());
    for (size_t i = 0; i < coverage.pairs().size(); ++i) {
      const auto &pair = coverage.pairs()[i];
      if (pair.receiver >= 8192 || pair.capture >= fits.attempts.size())
        throw std::invalid_argument("Invalid retained receiver coverage owner");
      result.observations.push_back({pair.receiver, pair.capture, coverage.visibility()[i]});
    }
    return result;
  }
  ReceiverCoveragePlan planReceiverCoverage(const scene::BakeScene &source,
      const CoverageBaselineSet &snapshot, const ReceiverFits &fits, const ReceiverAudit &coverage,
      std::stop_token cancelled) {
    return planReceiverCoverage(source, snapshot, retainReceiverCoverage(source, fits, coverage), cancelled);
  }
  ReceiverCoveragePlan planReceiverCoverage(const scene::BakeScene &source,
      const CoverageBaselineSet &snapshot, const ReceiverCoverageEvidence &evidence, std::stop_token cancelled,
      std::span<const uint32_t> eligibleCandidates) {
    if (!matchesCoverageContext(evidence.sourceContext, source, CoverageBaselineSet{}))
      throw std::invalid_argument("Retained receiver evidence belongs to a different frozen scene");
    const auto &fits = evidence.fits;
    const auto &baseline = snapshot.volumes;
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Local reflection coverage plan superseded");
    };
    check();
    if (snapshot.disabled || !snapshot.automaticAllowed || (snapshot.composed && baseline.empty())) {
      if (snapshot.disabled && !baseline.empty())
        throw std::invalid_argument("Disabled coverage baseline has active volumes");
      ReceiverCoveragePlan result;
      result.context = coveragePlanContext(source, snapshot);
      result.composedBaseline = snapshot.composed;
      result.disabledBaseline = snapshot.disabled;
      return result;
    }
    if (evidence.observations.size() > 32768 || baseline.size() > maxProbes || fits.attempts.size() > 32)
      throw std::invalid_argument("Invalid local coverage planning inputs");
    static_assert(8192 + maxProbes * coverageGuardsPerVolume <= maximumCoverageSamples);
    const auto guards = coverageGuardPoints(baseline, cancelled);
    const auto receivers = sampleReceivers(source, 8192, cancelled);
    ReceiverCoveragePlan result;
    result.context = coveragePlanContext(source, snapshot);
    result.surfaceSamples = uint32_t(receivers.size());
    result.composedBaseline = snapshot.composed;
    std::vector<float> areas;
    areas.reserve(receivers.size() + guards.size());
    std::vector<CoverageBaseline> current(baseline.size());
    for (size_t i = 0; i < baseline.size(); ++i) {
      const auto &volume = baseline[i];
      current[i].protectedCapture = volume.protectedCapture || (snapshot.composed && !volume.source.automatic);
    }
    for (uint32_t r = 0; r < receivers.size(); ++r) {
      check();
      areas.push_back(receivers[r].areaMetresSquared);
      for (uint32_t b = 0; b < baseline.size(); ++b) {
        const auto &capture = baseline[b];
        if (contains(capture.influenceBounds(),
                     projectionPoint(capture.record.worldToLocal, receivers[r].position))) current[b].receivers.push_back(r);
      }
    }
    // These points constrain preservation, but cannot earn surface-area gains.
    // New local captures have no volume-ray evidence for them, so only existing
    // baseline captures may provide their conservative coverage in this plan.
    for (const auto &position : guards) {
      check();
      const uint32_t sample = uint32_t(areas.size());
      areas.push_back(0);
      ++result.guardSamples;
      for (uint32_t b = 0; b < baseline.size(); ++b) {
        const auto &capture = baseline[b];
        if (contains(capture.influenceBounds(),
                     projectionPoint(capture.record.worldToLocal, position))) current[b].receivers.push_back(sample);
      }
    }
    std::vector<CoverageCandidate> candidates;
    std::vector<uint32_t> offsets;
    for (uint32_t a = 0; a < fits.attempts.size(); ++a) {
      offsets.push_back(uint32_t(candidates.size()));
      if (fits.attempts[a].variants.size() > ReceiverSiteFit::maximumVariants)
        throw std::invalid_argument("Local coverage planning variant budget exceeded");
      for (uint32_t v = 0; v < fits.attempts[a].variants.size(); ++v) {
        const auto &variant = fits.attempts[a].variants[v];
        if (!variant.geometry || !variant.geometry->geometryEvaluated)
          throw std::invalid_argument("Local coverage plan requires completed geometry");
        double error = 0, hitWeight = 0;
        for (const auto &face : variant.geometry->faces) {
          const double weight = double(face.solidAngleFraction) * (1 - face.missFraction);
          error += weight * face.meanAbsoluteRelativeResidual;
          hitWeight += weight;
        }
        const float residual = hitWeight > 0 ? float(error / hitWeight) : std::numeric_limits<float>::max();
        candidates.push_back({{}, residual});
        result.owners.push_back({a, v});
        result.errors.push_back(residual);
        Record record;
        for (uint32_t axis = 0; axis < 3; ++axis) {
          record.minimum[axis] = variant.projection.minimum[axis];
          record.maximum[axis] = variant.projection.maximum[axis];
          record.influenceMinimum[axis] = variant.influence.minimum[axis];
          record.influenceMaximum[axis] = variant.influence.maximum[axis];
          record.position[axis] = variant.geometry->site.position[axis];
        }
        result.candidateRecords.push_back(record);
      }
    }
    for (const auto &pair : evidence.observations) {
      check();
      if (pair.receiver >= receivers.size() || pair.capture >= fits.attempts.size())
        throw std::invalid_argument("Invalid local coverage observation owner");
      if (pair.visibility != ReceiverVisibility::Visible) continue;
      const auto &variants = fits.attempts[pair.capture].variants;
      for (uint32_t v = 0; v < variants.size(); ++v)
        if (contains(variants[v].influence, receivers[pair.receiver].position))
          candidates[offsets[pair.capture] + v].receivers.push_back(pair.receiver);
    }
    if (!eligibleCandidates.empty()) {
      std::vector<bool> eligible(candidates.size());
      for (auto candidate : eligibleCandidates) {
        if (candidate >= candidates.size() || eligible[candidate])
          throw std::invalid_argument("Invalid prepared local candidate eligibility");
        eligible[candidate] = true;
      }
      for (size_t i = 0; i < candidates.size(); ++i)
        if (!eligible[i]) candidates[i].error = std::numeric_limits<float>::max();
    }
    result.replacements = planCoverageReplacements(areas, current, candidates, result.maximumError, maxProbes, result.maximumChanges, cancelled);
    return result;
  }
}
