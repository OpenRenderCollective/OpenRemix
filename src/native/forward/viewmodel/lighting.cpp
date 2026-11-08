#include "forward/viewmodel/lighting.h"
#include <algorithm>
#include <cmath>

namespace remix::forward::viewmodel {
  Matrix sunProjection(const Bounds &receivers, const Bounds &casters, Vec3 direction,
                        uint32_t resolution) {
    if (!receivers.valid) return scene::identity();
    const auto z = normalized(direction);
    const auto x = normalized(cross(std::abs(z[2]) < .99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0}, z));
    const auto y = cross(z, x);
    Matrix basis = scene::identity();
    for (uint32_t c = 0; c < 3; ++c) {
      basis[c * 4] = x[c];
      basis[c * 4 + 1] = y[c];
      basis[c * 4 + 2] = z[c];
    }
    auto box = transformBounds(receivers, basis);
    const auto world = transformBounds(casters, basis);
    if (world.valid) {
      box.minimum[2] = std::min(box.minimum[2], world.minimum[2]);
      box.maximum[2] = std::max(box.maximum[2], world.maximum[2]);
    }
    Matrix projection = scene::identity();
    for (uint32_t c = 0; c < 3; ++c) {
      float span = std::max(box.maximum[c] - box.minimum[c], .01f);
      float centre = (box.minimum[c] + box.maximum[c]) * .5f;
      if (c < 2) {
        // A subtexel pose change must not resize the frozen-world map.
        const float quantum = std::ldexp(1.f, std::ilogb(span) - 7);
        span = std::ceil(span / quantum) * quantum;
        const auto mapSize = std::max(resolution, 8u);
        const auto snapTexels = std::clamp(mapSize / 16u, 1u, 4u);
        // The nearest grid point moves the center by at most half a step.
        // Leave two more texels on each side for the 4x4 tent.
        span *= float(mapSize) / float(mapSize - snapTexels - 4);
        const float snap = span * float(snapTexels) / float(mapSize);
        centre = std::round(centre / snap) * snap;
      } else {
        span += std::max(.01f, span * .01f);
      }
      projection[c * 4 + c] = (c == 2 ? 1.f : 2.f) / span;
      projection[12 + c] = -centre * projection[c * 4 + c] + (c == 2 ? .5f : 0.f);
    }
    return scene::multiply(projection, basis);
  }
}
