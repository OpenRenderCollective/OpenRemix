#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace remix::scene {
  // CPU-built VK_EXT_opacity_micromap input for the alpha-tested triangles of
  // one surface, independent of Vulkan. Triangle matches VkMicromapTriangleEXT;
  // states and special indices use the extension's 4-state encoding.
  struct OpacityMicromap {
    enum State : uint8_t { Transparent = 0, Opaque = 1, UnknownTransparent = 2, UnknownOpaque = 3 };
    // VK_OPACITY_MICROMAP_SPECIAL_INDEX_*_EXT.
    static constexpr int32_t FullyTransparent = -1, FullyOpaque = -2, FullyUnknownTransparent = -3,
                             FullyUnknownOpaque = -4;
    static constexpr uint16_t Format4State = 2; // VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT
    static constexpr uint32_t maxLevel = 8;     // Deepest subdivision the baker produces.
    struct Triangle {
      uint32_t dataOffset;
      uint16_t subdivisionLevel, format;
    };
    std::vector<Triangle> triangles;
    std::vector<uint8_t> data; // Two bits per micro-triangle, first in the low bits.
    // One entry per surface triangle: a Triangle index or a special index. Only
    // triangles the partition leaves non-opaque are traversed with it.
    std::vector<int32_t> indices;
    std::array<uint32_t, maxLevel + 1> levels{};       // Stored triangles per subdivision level.
    uint64_t hash = 0;                                 // Content identity of triangles, data and indices.
    uint64_t opaque = 0, transparent = 0, unknown = 0; // Micro-triangles, for statistics.
  };

  // Index of the micro-triangle at discrete barycentrics (u, v, w) in steps of
  // 2^-level along vertices 1, 2 and 0 (the hierarchical "bird curve" order;
  // checked against hardware by tools/vulkan-probes micromap mode).
  uint32_t microTriangleIndex(uint32_t u, uint32_t v, uint32_t w, uint32_t level);
  // A micro-triangle's lower corner in 2^-level steps along (u, v). Upright:
  // corners (i,j), (i+1,j), (i,j+1); inverted: (i+1,j), (i,j+1), (i+1,j+1).
  struct MicroTriangle {
    uint16_t i = 0, j = 0;
    bool upright = true;
  };
  // All micro-triangles of a level, indexed by microTriangleIndex. The children
  // of index k at level L are 4k..4k+3 at level L+1.
  const std::vector<MicroTriangle> &microTriangles(uint32_t level);
  // Barycentric (u, v) of vertices 1 and 2 at each corner.
  std::array<std::array<float, 2>, 3> microCorners(MicroTriangle triangle, uint32_t level);
} // namespace remix::scene
