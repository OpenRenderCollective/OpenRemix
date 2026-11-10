#pragma once
#include "forward/lighting.h"

namespace remix::forward::viewmodel {
  // One close-up sun cascade. Receiver bounds control XY resolution; world
  // casters extend depth only, so offscreen geometry can still shadow a weapon.
  Matrix sunProjection(const Bounds &receivers, const Bounds &casters, Vec3 direction,
                        uint32_t resolution);
}
