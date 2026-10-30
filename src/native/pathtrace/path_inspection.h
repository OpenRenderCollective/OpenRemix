#pragma once
#include "scene/types.h"
#include <array>
#include <cstdint>
#include <vector>

namespace remix {
  // One pixel's path, as the last frame traced it (PathTracer::inspectPixel):
  // records of 16 floats, kind in [3] (pathtrace.comp pathDebugRecord), and
  // the world-to-clip camera and render size they were traced with.
  struct PathInspection {
    std::vector<std::array<float, 16>> records;
    scene::Matrix camera{};
    uint32_t width = 0, height = 0, x = 0, y = 0;
    uint64_t frame = 0; // frames inspected so far (0: none yet)
  };
} // namespace remix
