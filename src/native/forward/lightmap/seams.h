#pragma once
#include "forward/lightmap/layout.h"

namespace remix::forward::lightmap {
  // Extend bilinear footprints only over verified connected coplanar geometry.
  // Copies real surface samples with the receiving chart's mixed-light ownership;
  // it never blends coefficient channels belonging to different lights.
  void extendSeams(const scene::BakeScene &scene, Layout &layout, uint32_t padding,
                   std::stop_token cancelled = {});
  // Follow actual triangle adjacency across curved joins. Open mesh edges stop
  // the walk; existing receiver samples remain available for visibility proofs.
  void extendCurvedSeams(const scene::BakeScene &scene, Layout &layout, uint32_t padding,
                         std::stop_token cancelled = {});
} // namespace remix::forward::lightmap
