#pragma once
#include "forward/bindings.h"

namespace remix::forward::water {
  // Measured, depth-writing bodies and finite sheets. Unsupported blend,
  // mask and overlay operations remain owned by compatibility rasterization.
  bool accepts(const scene::Draw &draw);
  struct MaterialData {
    // Body: RGB transmission / measurement distance.
    // Sheet: normal optical depth / thickness-to-measurement ratio.
    Vec4 transmittance{};
    // IOR, animation seconds, normal layer count, thickness (zero for bodies).
    Vec4 optical{};
    Vec4 scroll01{}, scroll2{}; // scroll2.zw: independent transmission-tint scroll
  };
  static_assert(sizeof(MaterialData) == 64);
  MaterialData materialData(const scene::Material &material, double seconds);
} // namespace remix::forward::water
