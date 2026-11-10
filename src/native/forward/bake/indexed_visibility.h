#pragma once
#include "forward/bake/gpu.h"

namespace remix::forward::bake {
  struct Transport::Gpu::IndexedVisibility {
    std::vector<Point> receivers;
  };
}
