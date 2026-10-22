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
    std::array<float, 4> depth{};
    std::array<float, 4> parameters{}; // IOR, thickness, thickness/measurement, reserved
    bool operator==(const SheetMaterial &) const = default;
  };
  static_assert(sizeof(SheetMaterial) == 32 && offsetof(SheetMaterial, parameters) == 16);

  inline SheetMaterial sheetMaterial(const scene::Material &material) {
    SheetMaterial result;
    if (const auto sheet = scene::measuredSheet(material)) {
      std::copy(sheet->normalDepth.begin(), sheet->normalDepth.end(), result.depth.begin());
      result.parameters = {material.refractiveIndex, material.thinWallThickness, sheet->thicknessRatio, 0};
    }
    return result;
  }
} // namespace remix::trace
