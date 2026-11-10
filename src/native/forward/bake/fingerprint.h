#pragma once
#include "forward/bake/grid.h"
#include "forward/lightmap/data.h"
#include "scene/light_bvh.h"
#include <cstddef>

namespace remix::forward::probes { struct Capture; }

namespace remix::forward::bake {
  // Ordered GPU input bytes after direct-light policy and light-tree packing.
  // Texture fingerprints are source identities, not a readback of the GPU atlas.
  void dumpInputFingerprint(uint64_t source, const Inputs &inputs, std::span<const scene::LightNode> nodes,
                            std::span<const scene::LightNode> componentNodes, const Frame &frame,
                            uint32_t resolution, float sunDistance) noexcept;
  // Diagnostic value only: no capture storage or provider lifetime is retained.
  struct ReflectionFingerprint {
    size_t count = 0;
    uint64_t radiance = 0, sites = 0, lighting = 0;
  };
  ReflectionFingerprint fingerprintCaptures(std::span<const probes::Capture> captures) noexcept;
  // Optional worker-side phase receipts. The environment variable names a JSONL
  // output file; disabled receipts do no hashing. Diagnostic failure cannot
  // abort publication. Fields are hashed separately to locate changed data.
  void dumpPhaseFingerprint(const char *phase, uint64_t source, const Grid &grid, const lightmap::Layout &layout,
                            std::span<const Result> surfaces, std::span<const lightmap::Sun> sun,
                            ReflectionFingerprint reflections) noexcept;
}
