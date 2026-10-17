#pragma once
#include "scene/bake_scene.h"
#include <stop_token>

namespace remix::forward::lightmap {
  struct CurvedCoordinates {
    std::vector<std::array<double, 2>> metres;
    bool intrinsic = false;
  };
  // Four-sided disk charts get an intrinsic harmonic layout. Other valid
  // topologies keep their authored layout with measured metric correction.
  // Both paths bound the largest world-space derivative by one metre per UV unit.
  CurvedCoordinates curvedCoordinates(const scene::BakeScene &scene, const scene::BakeSurfaceChart &chart,
                                      std::stop_token cancelled);
} // namespace remix::forward::lightmap
