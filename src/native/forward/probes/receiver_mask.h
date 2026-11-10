#pragma once
#include "forward/probes/data.h"

namespace remix::forward::probes {
  // Conservative receiver-volume overlap. Padding covers normal-biased queries;
  // uncertain bounds/transforms retain candidates. Bits preserve header order.
  std::array<uint32_t, 2> receiverMask(const Header &header, const Bounds &worldBounds, float padding);
}
