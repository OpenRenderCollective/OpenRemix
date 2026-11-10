#pragma once
#include "forward/bake/data.h"
#include <span>

namespace remix::forward::ray {
  struct Capture {
    std::shared_ptr<scene::BakeScene> source;
    bake::Inputs inputs;
    uint64_t key = 0;
  };
  // A transport snapshot, not a bake publication: current actors can move,
  // deform, animate and carry dynamic materials. Complete authored geometry
  // supplements the current submitted draws; live records override their own
  // authored instance so a changed object cannot appear twice.
  Capture capture(const scene::BakeScene *authored, std::span<const scene::Draw> current,
                  std::span<const scene::SphereLight> lights, const scene::DistantLight &sun,
                  const scene::DomeLight &dome, float sceneScale, double seconds = 0);
} // namespace remix::forward::ray
