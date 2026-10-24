#include "forward/probes/receiver_coverage_plan.h"
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
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Local reflection coverage plan superseded");
    };
    check();
    if (!coverage.complete() || baseline.size() > maxProbes || fits.attempts.size() > 32)
      throw std::invalid_argument("Invalid local coverage planning inputs");
    constexpr uint32_t axisSamples = 5, guardsPerCapture = 1 + axisSamples * axisSamples * axisSamples;
    static_assert(8192 + maxProbes * guardsPerCapture <= maximumCoverageSamples);
    const auto receivers = sampleReceivers(source, 8192, cancelled);
    ReceiverCoveragePlan result;
    result.surfaceSamples = uint32_t(receivers.size());
    std::vector<float> areas;
    areas.reserve(receivers.size() + baseline.size() * guardsPerCapture);
    std::vector<CoverageBaseline> current(baseline.size());
    for (const auto &capture : baseline) validateCapture(capture);
    for (uint32_t r = 0; r < receivers.size(); ++r) {
      check();
      areas.push_back(receivers[r].areaMetresSquared);
      for (uint32_t b = 0; b < baseline.size(); ++b) {
        const auto &capture = baseline[b];
        if (contains(capture.influence.valid ? capture.influence : capture.bounds,
                     projectionPoint(capture.worldToLocal, receivers[r].position))) current[b].receivers.push_back(r);
      }
    }
    // These points constrain preservation, but cannot earn surface-area gains.
    // New local captures have no volume-ray evidence for them, so only existing
    // baseline captures may provide their conservative coverage in this plan.
    auto guard = [&](Vec3 position) {
      check();
      for (float value : position)
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite reflection volume guard");
      const uint32_t sample = uint32_t(areas.size());
      areas.push_back(0);
      ++result.guardSamples;
      for (uint32_t b = 0; b < baseline.size(); ++b) {
        const auto &capture = baseline[b];
        if (contains(capture.influence.valid ? capture.influence : capture.bounds,
                     projectionPoint(capture.worldToLocal, position))) current[b].receivers.push_back(sample);
      }
    };
    for (const auto &capture : baseline) {
      guard(capture.position);
      const auto &bounds = capture.influence.valid ? capture.influence : capture.bounds;
      for (uint32_t x = 0; x < axisSamples; ++x)
        for (uint32_t y = 0; y < axisSamples; ++y)
          for (uint32_t z = 0; z < axisSamples; ++z) {
            const uint32_t cell[3]{x, y, z};
            Vec3 local{}, world{};
            for (uint32_t axis = 0; axis < 3; ++axis)
              local[axis] = float(double(bounds.minimum[axis]) +
                  (double(bounds.maximum[axis]) - bounds.minimum[axis]) * (cell[axis] + .5) / axisSamples);
            // Rigid inverse: transpose(R) * (local - translation).
            for (uint32_t c = 0; c < 3; ++c) {
              double coordinate = 0;
              for (uint32_t axis = 0; axis < 3; ++axis)
                coordinate += double(capture.worldToLocal[axis][c]) *
                              (double(local[axis]) - capture.worldToLocal[axis][3]);
              world[c] = float(coordinate);
            }
            guard(world);
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
      }
    }
    for (size_t i = 0; i < coverage.pairs().size(); ++i) {
      check();
      const auto &pair = coverage.pairs()[i];
      if (pair.receiver >= receivers.size() || pair.capture >= fits.attempts.size())
        throw std::invalid_argument("Invalid local coverage observation owner");
      if (coverage.visibility()[i] != ReceiverVisibility::Visible) continue;
      const auto &variants = fits.attempts[pair.capture].variants;
      for (uint32_t v = 0; v < variants.size(); ++v)
        if (contains(variants[v].influence, receivers[pair.receiver].position))
          candidates[offsets[pair.capture] + v].receivers.push_back(pair.receiver);
    }
    result.replacements = planCoverageReplacements(areas, current, candidates, result.maximumError, maxProbes, result.maximumChanges, cancelled);
    return result;
  }
}
