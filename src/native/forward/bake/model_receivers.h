#pragma once
#include "forward/bake/data.h"
#include <stop_token>
#include <vector>

namespace remix::forward::bake {
  struct ModelReceiver {
    Point point;
    uint32_t draw = 0, firstIndex = 0;
  };
  // Representative uncharted rigid-opaque triangle centres. Up to six normal
  // directions per draw; budget rounds across draws before taking more faces.
  // Closed captured partitions exclude centres inside solid/unplayable leaves.
  // These are geometry samples, not independent lighting references.
  std::vector<ModelReceiver> modelReceivers(const scene::BakeScene &source,
      uint32_t maximumSamples = 4096, std::stop_token cancelled = {});
}
