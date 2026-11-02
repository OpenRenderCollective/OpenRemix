#pragma once
#include "scene/bake_scene.h"
#include <memory>
#include <span>

namespace remix::forward::shadows {
  // Static shadow casters when the host supplies no ready bake scene: the
  // frame's static-classified opaque world draws, as a ready scene whose draws
  // and bindings match them (scene::matchesBakeDraw). Frozen shadow maps then
  // reuse them exactly as for a host scene. CPU-only.
  //
  // Identity of that draw set (surfaces, materials, transforms and ranges);
  // zero when it is empty. Rebuild the scene only when this changes.
  uint64_t staticCasterKey(std::span<const scene::Draw> draws);
  std::shared_ptr<const scene::BakeScene> implicitStaticScene(std::span<const scene::Draw> draws,
                                                              float sceneScale);
} // namespace remix::forward::shadows
