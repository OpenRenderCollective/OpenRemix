#pragma once
#include "forward/lighting.h"
#include <cstdint>
#include <vector>

namespace remix::forward {
  struct Options;
  // CPU-only lighting environment shared by the sky, shading and radiance
  // cascades. No Vulkan state: textures are uploaded by the caller.
  namespace sky_model {
    // World to the dome's Y-up frame (column-major), for a world up axis
    // 0 X, 1 Y or 2 Z. The equirectangular lookup matches forward/sky.cpp.
    Matrix worldToDome(uint32_t upAxis);
    // The live distant light: the scene's, or the panel's override. The
    // override's direction is built around the same up axis as the sky.
    scene::DistantLight effectiveSun(const scene::DistantLight &scene, const Options &options, uint32_t upAxis);
    // Radiance scale of the procedural sky's unit texels: proportional to the
    // sun's irradiance (a clear sky adds about a sixth of it on the ground).
    float radianceScale(const scene::DistantLight &sun, const Options &options);
    constexpr uint32_t width = 256, height = 128;
    // RGBA16F texels of a clear analytic sky around the dome-space direction
    // toward the sun (zenith about 1, ground below the horizon). Equirectangular
    // u = atan(z, x) / 2pi + 1/2, v = acos(y) / pi.
    std::vector<uint16_t> texels(const Vec3 &towardSun);
  } // namespace sky_model
} // namespace remix::forward
