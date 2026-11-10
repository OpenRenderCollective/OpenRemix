#pragma once
#include "forward/probes/data.h"

namespace remix::forward::probes {
  struct LayerCopy {
    bool automatic = false;
    uint32_t layer = 0;
  };
  struct Composition {
    Header header;
    std::vector<LayerCopy> layers;
    uint32_t automatic = 0, covered = 0, omitted = 0;
  };
  // Keep every authored record; retain automatic order outside proven coverage.
  // The output is a priority-ordered GPU copy plan, never a new radiance bake.
  Composition compose(const Header &authored, const Header &automatic, uint32_t capacity = maxProbes);
}
