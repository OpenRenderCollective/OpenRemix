#pragma once
#include <array>
#include <cstdint>

namespace remix {
  // remixapi_VolumeShaftEXT as validated (NativeResources::drawVolumeShafts):
  // an orthonormal frame (u, v across, axis along), the near cap's half
  // extents and the far cap's, its glow per world unit at the near cap.
  struct VolumeShaft {
    std::array<float, 3> origin{}, axis{}, u{}, v{};
    float length = 0;
    std::array<float, 2> nearHalf{}, farHalf{};
    std::array<float, 3> radiance{};
    float farFade = 1, softness = 0;
    uint32_t shape = 0;
    uint32_t flags = 0; // REMIXAPI_VOLUME_SHAFT_SHADOWED_EXT
  };
} // namespace remix
