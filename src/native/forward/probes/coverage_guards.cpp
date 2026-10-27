#include "forward/probes/coverage_guards.h"
#include <cmath>
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
}
