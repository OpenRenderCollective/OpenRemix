#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Acceleration structures for devices without ray queries, built on the CPU
// with TinyBVH and traversed by shaders/software_ray_query.glsl. CPU only.
//
// Both structures are arrays of 16-byte words (four floats or four uints):
//
//   word 0          root reference, first leaf-item word, then the address of
//                   word 0 itself (low, high): setAddress, for a top level
//   words 1..       nodes of a two-child BVH, four words each:
//                     left child low,  left child reference
//                     left child high, right child reference
//                     right child low,  0
//                     right child high, 0
//   then the leaf items, in leaf order:
//     bottom level  triangles, three words each:
//                     vertex 0,            primitive index (within its geometry)
//                     vertex 1 - vertex 0, geometry index | opaque << 31
//                     vertex 2 - vertex 0, 0
//     top level     instances, eight words each:
//                     world to object, three rows of four
//                     object to world, three rows of four
//                     custom index, mask, flags, 0
//                     bottom level address (low, high), 0, 0
//
// A reference is a node index, or a leaf: leafBit | count << 24 | first item.
// emptyReference is a structure with nothing in it.
//
// Measured on a Quadro T1000 before settling on this: a compressed
// eight-wide bottom level (TinyBVH's BVH8_CWBVH) traced 40% more rays alone
// but no more in the path tracer's passes (8% fewer on small meshes), and
// its refit costs thirty times this one's; spatial splits bought no rays
// for two to three times the build.
namespace remix::trace::software {
  constexpr uint32_t leafBit = 0x80000000u, emptyReference = 0xffffffffu;
  constexpr uint32_t leafCountLimit = 127, leafFirstLimit = 0xffffffu;
  constexpr uint32_t nodeWords = 4, triangleWords = 3, instanceWords = 8;
  constexpr uint32_t opaqueBit = 0x80000000u;

  // One geometry of a bottom level: `triangles` index triples into positions
  // (three floats each).
  struct Geometry {
    const float *positions = nullptr;
    const uint32_t *indices = nullptr;
    uint32_t triangles = 0;
    bool opaque = false;
  };
  struct Structure {
    std::vector<uint32_t> words; // four per 16-byte word
    float low[3] = {}, high[3] = {};
    uint32_t depth = 0; // the most nodes on a path to a leaf
    size_t bytes() const { return words.size() * sizeof(uint32_t); }
    void setAddress(uint64_t address) {
      words[2] = uint32_t(address);
      words[3] = uint32_t(address >> 32);
    }
  };
  // Throws std::length_error over leafFirstLimit triangles.
  Structure buildBottom(const Geometry *geometries, size_t count);

  // The bottom level of geometry that deforms: update() refits the last
  // build's tree (a fiftieth of a build's time) while `topology`, the
  // triangles and their order stay the same, at most refitLimit times in a
  // row (0: without limit), and builds otherwise. One thread at a time.
  class BottomBuilder {
  public:
    BottomBuilder();
    ~BottomBuilder();
    Structure update(const Geometry *geometries, size_t count, uint64_t topology, uint32_t refitLimit);
    bool refitted() const { return m_refitted; } // the last update

  private:
    struct State;
    std::unique_ptr<State> m_state;
    bool m_refitted = false;
  };

  struct Instance {
    float transform[3][4] = {}; // object to world, as VkTransformMatrixKHR
    uint32_t customIndex = 0, mask = 0, flags = 0;
    uint64_t bottom = 0;             // address of its bottom level's words; 0: none
    float low[3] = {}, high[3] = {}; // the bottom level's bounds
  };
  // Instances without a bottom level, with empty bounds or with a singular
  // transform are left out.
  Structure buildTop(const Instance *instances, size_t count);

  // A top level kept between builds: update() with `refit` keeps the last
  // build's tree for the same instances moved, and builds otherwise.
  class TopBuilder {
  public:
    TopBuilder();
    ~TopBuilder();
    Structure update(const Instance *instances, size_t count, bool refit);
    bool refitted() const { return m_refitted; }

  private:
    struct State;
    std::unique_ptr<State> m_state;
    bool m_refitted = false;
  };
} // namespace remix::trace::software
