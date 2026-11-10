#pragma once
#include "scene/bake_scene.h"
#include <stop_token>

namespace remix::forward::lightmap {
  struct CurvedCoordinates {
    std::vector<std::array<double, 2>> metres;
    bool intrinsic = false;
  };
  // Four-sided disks use a harmonic layout; open fans can unfold exactly.
  // Other valid topologies retain their authored layout with metric correction.
  // Both paths bound the largest world-space derivative by one metre per UV unit.
  CurvedCoordinates curvedCoordinates(const scene::BakeScene &scene, const scene::BakeSurfaceChart &chart,
                                      std::stop_token cancelled);
} // namespace remix::forward::lightmap
