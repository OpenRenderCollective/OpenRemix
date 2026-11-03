#pragma once
#include "forward/lighting.h"

namespace remix::forward::cascades {
  // Optional std140 tail of the scene uniform. Only cascade receiver shaders
  // declare it; the common frame layout and inactive-provider upload stay fixed.
  struct ReceiverData {
    Matrix camera{};
    Vec4 control{}; // enabled, intensity, depth tolerance, probe spacing
  };
  static_assert(sizeof(ReceiverData) == 80);
}
