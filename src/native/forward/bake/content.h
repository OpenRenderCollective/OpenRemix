#pragma once
#include <cstdint>

namespace remix::forward::bake {
  // Fingerprints of captured radiance and the packed payloads published to the
  // GPU. Zero denotes absent data; these are diagnostics, not persistent cache keys.
  struct ContentHashes {
    uint64_t grid = 0, reflections = 0, surfaces = 0;
    uint64_t staticOcclusion = 0;
    uint64_t captureRadiance = 0;
    uint64_t captureInputs = 0;
    uint64_t styledIndirect = 0;
    bool operator==(const ContentHashes &) const = default;
  };
} // namespace remix::forward::bake
