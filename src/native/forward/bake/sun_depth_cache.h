#pragma once
#include "forward/bake/cache_io.h"
#include "forward/bake/sun_depth_layout.h"
namespace remix::forward::bake::sun_depth_cache {
  // Versioned chunk inside the existing checksummed cache container. Layout and
  // sampling identity come from the current scene, never from unchecked counts.
  // The container key must also include scene and capture-kernel policy identity.
  // Only captured tile rows are serialized; unused atlas space restores to -1.
  // Captured -2 samples preserve unresolved-transmission fallback requests.
  void write(cache::detail::Writer &output, const SunDepthLayout &layout, const SunDepthHeader &header,
             std::span<const float> atlas, std::stop_token cancelled = {});
  std::vector<float> read(cache::detail::Reader &input, const SunDepthLayout &layout,
                         const SunDepthHeader &header, std::stop_token cancelled = {});
}
