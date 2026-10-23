#include "forward/probes/receiver_fit_pairs.h"
#include <algorithm>

namespace remix::forward::probes {
  namespace {
    bool contains(const Bounds &box, Vec3 position) {
      for (uint32_t c = 0; c < 3; ++c)
        if (position[c] < box.minimum[c] || position[c] > box.maximum[c]) return false;
      return true;
    }
  }
  std::vector<ReceiverPair> receiverFitPairs(std::span<const ReceiverSample> receivers,
      std::span<const Capture> baseline, const ReceiverFits &fits, float bias,
      uint32_t budget, std::stop_token cancelled) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Local receiver coverage superseded");
    };
    check();
    if (receivers.size() > 8192 || baseline.size() > maxProbes || fits.attempts.size() > 32 ||
        !budget || budget > 32768 || !std::isfinite(bias) || bias <= 0)
      throw std::invalid_argument("Invalid local receiver coverage budget");
    for (const auto &capture : baseline) validateCapture(capture);
    std::vector<Vec3> origins;
    for (const auto &fit : fits.attempts) {
      check();
      if (fit.variants.size() > 3) throw std::invalid_argument("Local fit variant budget exceeded");
      Vec3 origin{};
      for (size_t i = 0; i < fit.variants.size(); ++i) {
        const auto &v = fit.variants[i];
        if (!v.geometry || !v.geometry->geometryEvaluated || !v.influence.valid)
          throw std::invalid_argument("Local receiver coverage requires assessed fits");
        for (uint32_t c = 0; c < 3; ++c)
          if (!std::isfinite(v.influence.minimum[c]) || !std::isfinite(v.influence.maximum[c]) ||
              v.influence.minimum[c] >= v.influence.maximum[c] || !std::isfinite(v.geometry->site.position[c]))
            throw std::invalid_argument("Invalid local receiver influence or origin");
        if (i && origin != v.geometry->site.position)
          throw std::invalid_argument("Local fit variants must share one origin");
        origin = v.geometry->site.position;
      }
      origins.push_back(origin);
    }
    // Only small owner indices are retained for the full bounded Cartesian scan.
    // Geometry rays are allocated for the selected cohort, never for the pool.
    std::vector<std::pair<uint32_t, uint32_t>> pool;
    for (uint32_t r = 0; r < receivers.size(); ++r) {
      check();
      const auto &receiver = receivers[r];
      for (uint32_t c = 0; c < 3; ++c)
        if (!std::isfinite(receiver.position[c]) || !std::isfinite(receiver.normal[c]))
          throw std::invalid_argument("Nonfinite local coverage receiver");
      const bool covered = std::any_of(baseline.begin(), baseline.end(), [&](const Capture &capture) {
        return contains(capture.influence.valid ? capture.influence : capture.bounds,
                        projectionPoint(capture.worldToLocal, receiver.position));
      });
      if (covered) continue;
      for (uint32_t f = 0; f < fits.attempts.size(); ++f)
        if (std::any_of(fits.attempts[f].variants.begin(), fits.attempts[f].variants.end(),
                       [&](const ReceiverProjection &v) { return contains(v.influence, receiver.position); }) &&
            receiverRay(receiver, origins[f], bias)) pool.emplace_back(r, f);
    }
    std::vector<ReceiverPair> result;
    const size_t count = std::min(pool.size(), size_t(budget));
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      check();
      const auto [receiver, fit] = pool[((2 * i + 1) * pool.size()) / (2 * count)];
      result.push_back({receiver, fit, *receiverRay(receivers[receiver], origins[fit], bias)});
    }
    return result;
  }
}
