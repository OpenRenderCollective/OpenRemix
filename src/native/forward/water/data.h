#pragma once
#include "forward/bindings.h"

namespace remix::forward::water {
  // A measured, depth-writing body. Unsupported blend/mask/overlay operations
  // remain owned by compatibility rasterization.
  bool accepts(const scene::Draw &draw);
  struct MaterialData {
    Vec4 transmittance{}; // RGB transmission at the measurement distance in w
    Vec4 optical{};       // refractive index, animation seconds, normal layer count, reserved
    Vec4 scroll01{}, scroll2{};
  };
  static_assert(sizeof(MaterialData) == 64);
  MaterialData materialData(const scene::Material &material, double seconds);
} // namespace remix::forward::water
