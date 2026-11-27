#pragma once
#include "forward/bake/data.h"
#include <span>

namespace remix::forward::ray {
  struct Capture {
    std::shared_ptr<scene::BakeScene> source;
    bake::Inputs inputs;
    uint64_t key = 0;
    // Identity of everything packing reads, without packing the geometry.
    uint64_t signature = 0;
  };
  // A transport snapshot, not a bake publication: current actors can move,
  // deform, animate and carry dynamic materials. Complete authored geometry
  // supplements the current submitted draws; live records override their own
  // authored instance so a changed object cannot appear twice.
  Capture capture(const scene::BakeScene *authored, std::span<const scene::Draw> current,
                  std::span<const scene::SphereLight> lights, const scene::DistantLight &sun,
                  const scene::DomeLight &dome, float sceneScale, double seconds = 0,
                  const Capture *previous = nullptr);
  // `previous` is an earlier capture that the caller still retains, whose
  // transport is current. When this frame's signature matches it, the result
  // reuses its key and carries no geometry: only materials, textures and lights.
  // Retaining its source keeps every surface alive, so a matching surface
  // address is the same immutable surface and not a reused allocation.
} // namespace remix::forward::ray
