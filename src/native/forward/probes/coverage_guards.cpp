#include "forward/probes/coverage_guards.h"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace remix::forward::probes {
  std::vector<Vec3> coverageGuardPoints(std::span<const CoverageVolume> volumes, std::stop_token cancelled) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Reflection coverage guards superseded");
    };
    check();
    if (volumes.size() > maxProbes) throw std::invalid_argument("Too many coverage guard volumes");
    std::vector<Vec3> points;
    points.reserve(volumes.size() * coverageGuardsPerVolume);
    auto append = [&](Vec3 position) {
      check();
      for (float value : position)
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite reflection volume guard");
      points.push_back(position);
    };
    for (const auto &volume : volumes) {
      const auto bounds = volume.influenceBounds();
      if (!validProjectionSpace(volume.record.worldToLocal))
        throw std::invalid_argument("Invalid coverage volume frame");
      for (uint32_t axis = 0; axis < 3; ++axis)
        if (!std::isfinite(bounds.minimum[axis]) || !std::isfinite(bounds.maximum[axis]) ||
            bounds.minimum[axis] >= bounds.maximum[axis])
          throw std::invalid_argument("Invalid coverage volume bounds");
      append(volume.position());
      for (uint32_t x = 0; x < coverageGuardAxisSamples; ++x)
        for (uint32_t y = 0; y < coverageGuardAxisSamples; ++y)
          for (uint32_t z = 0; z < coverageGuardAxisSamples; ++z) {
            const uint32_t cell[3]{x, y, z};
            Vec3 local{}, world{};
            for (uint32_t axis = 0; axis < 3; ++axis)
              local[axis] = float(double(bounds.minimum[axis]) +
                  (double(bounds.maximum[axis]) - bounds.minimum[axis]) *
                      (cell[axis] + .5) / coverageGuardAxisSamples);
            // Rigid inverse: transpose(R) * (local - translation).
            for (uint32_t c = 0; c < 3; ++c) {
              double coordinate = 0;
              for (uint32_t axis = 0; axis < 3; ++axis)
                coordinate += double(volume.record.worldToLocal[axis][c]) *
                              (double(local[axis]) - volume.record.worldToLocal[axis][3]);
              world[c] = float(coordinate);
            }
            append(world);
          }
    }
    return points;
  }
  std::vector<ReceiverPair> coverageGuardPairs(std::span<const Vec3> guards, const ReceiverFits &fits,
      float minimumDistance, uint32_t budget, std::stop_token cancelled) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Reflection guard queries superseded");
    };
    check();
    if (guards.size() > maxProbes * coverageGuardsPerVolume || fits.attempts.size() > 32 ||
        !budget || budget > 32768 || !std::isfinite(minimumDistance) || minimumDistance <= 0)
      throw std::invalid_argument("Invalid reflection guard query budget");
    std::vector<Vec3> origins;
    for (const auto &fit : fits.attempts) {
      if (fit.variants.size() > ReceiverSiteFit::maximumVariants)
        throw std::invalid_argument("Too many reflection guard variants");
      Vec3 origin{};
      for (size_t i = 0; i < fit.variants.size(); ++i) {
        const auto &variant = fit.variants[i];
        if (!variant.geometry || !variant.geometry->geometryEvaluated || !variant.influence.valid)
          throw std::invalid_argument("Guard queries require assessed projection fits");
        for (uint32_t c = 0; c < 3; ++c)
          if (!std::isfinite(variant.influence.minimum[c]) || !std::isfinite(variant.influence.maximum[c]) ||
              variant.influence.minimum[c] >= variant.influence.maximum[c] ||
              !std::isfinite(variant.geometry->site.position[c]))
            throw std::invalid_argument("Invalid reflection guard fit");
        if (i && origin != variant.geometry->site.position)
          throw std::invalid_argument("Guard variants must share a capture origin");
        origin = variant.geometry->site.position;
      }
      origins.push_back(origin);
    }
    auto ray = [&](uint32_t guard, uint32_t capture) -> std::optional<ReceiverRay> {
      // These are volume points, not surfaces: do not offset the origin past
      // a nearby thin occluder. Degenerate/short connections stay unknown.
      ReceiverRay result{};
      double distanceSquared = 0;
      std::array<double, 3> delta;
      for (uint32_t c = 0; c < 3; ++c) {
        result.point.position[c] = guards[guard][c];
        delta[c] = double(origins[capture][c]) - guards[guard][c];
        distanceSquared += delta[c] * delta[c];
      }
      const double distance = std::sqrt(distanceSquared);
      result.distance = float(distance);
      if (!std::isfinite(result.distance) || distance <= minimumDistance) return {};
      for (uint32_t c = 0; c < 3; ++c) result.point.normal[c] = float(delta[c] / distance);
      return result;
    };
    std::vector<std::pair<uint32_t, uint32_t>> pool;
    for (uint32_t g = 0; g < guards.size(); ++g) {
      check();
      for (float value : guards[g])
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite reflection query guard");
      for (uint32_t f = 0; f < fits.attempts.size(); ++f) {
        const bool inside = std::any_of(fits.attempts[f].variants.begin(), fits.attempts[f].variants.end(),
            [&](const ReceiverProjection &variant) {
              for (uint32_t c = 0; c < 3; ++c)
                if (guards[g][c] < variant.influence.minimum[c] || guards[g][c] > variant.influence.maximum[c]) return false;
              return true;
            });
        if (inside && ray(g, f)) pool.emplace_back(g, f);
      }
    }
    const size_t count = std::min(pool.size(), size_t(budget));
    std::vector<ReceiverPair> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      check();
      const auto [guard, capture] = pool[((2 * i + 1) * pool.size()) / (2 * count)];
      result.push_back({guard, capture, *ray(guard, capture)});
    }
    return result;
  }
}
