#pragma once
#include "forward/bake/grid.h"

namespace remix::forward::bake {
  // Marks complete interpolation cells around true surface receivers. The
  // halo is in source world units. Solid classification is deliberately left
  // to placement; this describes geometric support, not visibility.
  std::vector<uint8_t> receiverSupport(const GridHeader &header, const lightmap::Layout &layout,
                                      float halo, std::stop_token cancelled = {});
  // Receiver bins can straddle a patch boundary. Return nearby source indices
  // in their original order so relocation sees the same candidates on both sides.
  std::vector<uint32_t> nearbyReceivers(const GridHeader &header, const lightmap::Layout &layout,
                                       std::span<const std::vector<uint32_t>> bins, uint32_t cell,
                                       float halo, std::stop_token cancelled = {});
}
