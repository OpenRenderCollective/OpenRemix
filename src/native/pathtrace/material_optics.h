#pragma once
#include "scene/optics.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace remix::trace {
  constexpr uint32_t measuredSheetMaterialFlag = 256;
  // Private, full-precision sheet data shared by persistent and rebuilt tables.
  // Zero thickness keeps legacy alpha sheets and containing bodies unchanged.
  struct SheetMaterial {
    std::array<float, 4> depth{}; // normal optical depth RGB, tint scroll U
    std::array<float, 4> parameters{}; // IOR, thickness, thickness/measurement, tint scroll V
    bool operator==(const SheetMaterial &) const = default;
  };
  static_assert(sizeof(SheetMaterial) == 32 && offsetof(SheetMaterial, parameters) == 16);

  inline SheetMaterial sheetMaterial(const scene::Material &material) {
    SheetMaterial result;
    if (const auto sheet = scene::measuredSheet(material)) {
      std::copy(sheet->normalDepth.begin(), sheet->normalDepth.end(), result.depth.begin());
      result.parameters = {material.refractiveIndex, material.thinWallThickness, sheet->thicknessRatio, 0};
      result.depth[3] = material.transmittanceScroll[0];
      result.parameters[3] = material.transmittanceScroll[1];
    }
    return result;
  }
} // namespace remix::trace
