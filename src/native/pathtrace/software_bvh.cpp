#include "pathtrace/software_bvh.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

// Builds run one per worker (PathTracer::Gpu::convertMesh), each on its own
// thread; only single precision triangles and boxes are built.
#define NO_THREADED_BUILDS
#define NO_DOUBLE_PRECISION_SUPPORT
#define NO_CUSTOM_GEOMETRY
#define NO_VOXEL_SUPPORT
#define TINYBVH_IMPLEMENTATION
#include <tiny_bvh.h>

namespace remix::trace::software {
  namespace {
    uint32_t bits(float value) {
      uint32_t out;
      std::memcpy(&out, &value, sizeof out);
      return out;
    }
    void put(std::vector<uint32_t> &words, size_t word, const float *xyz, uint32_t w) {
      for (unsigned c = 0; c < 3; ++c)
        words[word * 4 + c] = bits(xyz[c]);
      words[word * 4 + 3] = w;
    }

    // Writes a TinyBVH tree's nodes after word 0 and returns the root's
    // reference. Leaves refer to primIdx positions, the order of the items.
    struct Packer {
      const tinybvh::BVH &bvh;
      std::vector<uint32_t> &words;
      uint32_t nodes = 0, depth = 0;

      uint32_t node() {
        words.resize(words.size() + nodeWords * 4);
        return nodes++;
      }
      void child(uint32_t parent, unsigned side, const tinybvh::bvhvec3 &low, const tinybvh::bvhvec3 &high,
                 uint32_t reference) {
        const size_t base = 1 + size_t(parent) * nodeWords;
        const float l[3] = {low.x, low.y, low.z}, h[3] = {high.x, high.y, high.z};
        put(words, base + side * 2, l, side ? 0 : reference);
        put(words, base + side * 2 + 1, h, 0);
        if (side)
          words[(base + 1) * 4 + 3] = reference;
      }
      // A leaf's items, split into leaves of at most leafCountLimit.
      uint32_t leaf(uint32_t first, uint32_t count, const tinybvh::bvhvec3 &low, const tinybvh::bvhvec3 &high,
                    uint32_t level) {
        if (first + count - 1 > leafFirstLimit)
          throw std::length_error("software acceleration structure: too many items");
        if (count <= leafCountLimit) {
          depth = std::max(depth, level);
          return leafBit | count << 24 | first;
        }
        const uint32_t out = node(), half = count / 2;
        const uint32_t left = leaf(first, half, low, high, level + 1);
        const uint32_t right = leaf(first + half, count - half, low, high, level + 1);
        child(out, 0, low, high, left);
        child(out, 1, low, high, right);
        return out;
      }
      uint32_t pack(uint32_t index, uint32_t level) {
        const auto &source = bvh.bvhNode[index];
        if (source.isLeaf())
          return leaf(source.leftFirst, source.triCount, source.aabbMin, source.aabbMax, level);
        const uint32_t out = node();
        for (unsigned side = 0; side < 2; ++side) {
          const auto &next = bvh.bvhNode[source.leftFirst + side];
          const uint32_t reference = pack(source.leftFirst + side, level + 1);
          child(out, side, next.aabbMin, next.aabbMax, reference);
        }
        return out;
      }
    };

    Structure empty() {
      Structure out;
      out.words = {emptyReference, 1, 0, 0};
      return out;
    }
    void finish(Structure &out, const tinybvh::BVH &bvh, uint32_t root, uint32_t nodes) {
      out.words[0] = root;
      out.words[1] = 1 + nodes * nodeWords;
      out.words[2] = out.words[3] = 0;
      const auto &top = bvh.bvhNode[0];
      const float low[3] = {top.aabbMin.x, top.aabbMin.y, top.aabbMin.z};
      const float high[3] = {top.aabbMax.x, top.aabbMax.y, top.aabbMax.z};
      std::copy_n(low, 3, out.low);
      std::copy_n(high, 3, out.high);
    }
  } // namespace

  namespace {
    // TinyBVH's input: three padded vertices per triangle. `source` keeps
    // where each came from: its primitive index, and its geometry and flag.
    void gather(const Geometry *geometries, size_t count, std::vector<tinybvh::bvhvec4> &vertices,
                std::vector<uint32_t> &source) {
      size_t total = 0;
      for (size_t g = 0; g < count; ++g)
        total += geometries[g].triangles;
      if (total > leafFirstLimit)
        throw std::length_error("software acceleration structure: too many triangles");
      vertices.clear();
      source.clear();
      vertices.reserve(total * 3);
      source.reserve(total * 2);
      for (size_t g = 0; g < count; ++g) {
        const auto &geometry = geometries[g];
        for (uint32_t t = 0; t < geometry.triangles; ++t) {
          bool finite = true;
          tinybvh::bvhvec4 corner[3];
          for (unsigned v = 0; v < 3; ++v) {
            const float *p = geometry.positions + size_t(geometry.indices[t * 3 + v]) * 3;
            corner[v] = tinybvh::bvhvec4(p[0], p[1], p[2], 0);
            finite &= std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
          }
          // As the hardware build: a triangle with a NaN vertex is inactive.
          if (!finite)
            continue;
          vertices.insert(vertices.end(), corner, corner + 3);
          source.push_back(t);
          source.push_back(uint32_t(g) | (geometry.opaque ? opaqueBit : 0u));
        }
      }
    }

    // A two-child tree and the triangles in its leaves' order: vertex,
    // edges, and what the shaders ask of a hit.
    Structure packBottom(const tinybvh::BVH &bvh, const std::vector<tinybvh::bvhvec4> &vertices,
                         const std::vector<uint32_t> &source) {
      const auto triangles = uint32_t(source.size() / 2);
      Structure out;
      out.words.resize(4);
      Packer packer{bvh, out.words};
      const uint32_t root = packer.pack(0, 1);
      out.depth = packer.depth;
      finish(out, bvh, root, packer.nodes);
      const size_t base = out.words.size() / 4;
      out.words.resize(out.words.size() + size_t(bvh.idxCount) * triangleWords * 4);
      for (uint32_t i = 0; i < bvh.idxCount; ++i) {
        const uint32_t t = bvh.primIdx[i];
        if (t >= triangles)
          continue;
        const auto &a = vertices[t * 3], &b = vertices[t * 3 + 1], &c = vertices[t * 3 + 2];
        const float origin[3] = {a.x, a.y, a.z};
        const float first[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
        const float second[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
        const size_t word = base + size_t(i) * triangleWords;
        put(out.words, word, origin, source[t * 2]);
        put(out.words, word + 1, first, source[t * 2 + 1]);
        put(out.words, word + 2, second, 0);
      }
      return out;
    }

    // The instances a top level holds, with their inverse transforms, and
    // their world bounds (low, high) for TinyBVH.
    struct Kept {
      const Instance *instance;
      double inverse[3][4];
    };
    void collect(const Instance *instances, size_t count, std::vector<Kept> &kept,
                 std::vector<tinybvh::bvhvec4> &boxes) {
      kept.clear();
      boxes.clear();
      kept.reserve(count);
      boxes.reserve(count * 2);
      for (size_t i = 0; i < count; ++i) {
        const auto &instance = instances[i];
        if (!instance.bottom || !(instance.low[0] <= instance.high[0]) || !(instance.low[1] <= instance.high[1]) ||
            !(instance.low[2] <= instance.high[2]))
          continue;
        const auto &m = instance.transform;
        const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0],
                     h = m[2][1], k = m[2][2];
        const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
        if (!std::isfinite(det) || std::abs(det) < std::numeric_limits<double>::min())
          continue;
        Kept entry{&instance, {}};
        const double r[3][3] = {{(e * k - f * h) / det, (c * h - b * k) / det, (b * f - c * e) / det},
                                {(f * g - d * k) / det, (a * k - c * g) / det, (c * d - a * f) / det},
                                {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}};
        for (unsigned row = 0; row < 3; ++row) {
          for (unsigned column = 0; column < 3; ++column)
            entry.inverse[row][column] = r[row][column];
          entry.inverse[row][3] = -(r[row][0] * m[0][3] + r[row][1] * m[1][3] + r[row][2] * m[2][3]);
        }
        float low[3], high[3];
        std::fill_n(low, 3, std::numeric_limits<float>::max());
        std::fill_n(high, 3, std::numeric_limits<float>::lowest());
        for (unsigned corner = 0; corner < 8; ++corner) {
          const float p[3] = {corner & 1 ? instance.high[0] : instance.low[0],
                              corner & 2 ? instance.high[1] : instance.low[1],
                              corner & 4 ? instance.high[2] : instance.low[2]};
          for (unsigned row = 0; row < 3; ++row) {
            const float value = m[row][0] * p[0] + m[row][1] * p[1] + m[row][2] * p[2] + m[row][3];
            low[row] = std::min(low[row], value);
            high[row] = std::max(high[row], value);
          }
        }
        if (!std::isfinite(low[0] + low[1] + low[2] + high[0] + high[1] + high[2]))
          continue;
        kept.push_back(entry);
        boxes.emplace_back(low[0], low[1], low[2], 0.f);
        boxes.emplace_back(high[0], high[1], high[2], 0.f);
      }
    }

    Structure packTop(const tinybvh::BVH &bvh, const std::vector<Kept> &kept) {
      Structure out;
      out.words.resize(4);
      Packer packer{bvh, out.words};
      const uint32_t root = packer.pack(0, 1);
      out.depth = packer.depth;
      finish(out, bvh, root, packer.nodes);
      const size_t base = out.words.size() / 4;
      out.words.resize(out.words.size() + size_t(bvh.idxCount) * instanceWords * 4);
      for (uint32_t i = 0; i < bvh.idxCount; ++i) {
        const auto &entry = kept[bvh.primIdx[i]];
        const auto &instance = *entry.instance;
        uint32_t *word = out.words.data() + (base + size_t(i) * instanceWords) * 4;
        for (unsigned row = 0; row < 3; ++row)
          for (unsigned column = 0; column < 4; ++column) {
            word[row * 4 + column] = bits(float(entry.inverse[row][column]));
            word[12 + row * 4 + column] = bits(instance.transform[row][column]);
          }
        word[24] = instance.customIndex;
        word[25] = instance.mask;
        word[26] = instance.flags;
        word[27] = 0;
        word[28] = uint32_t(instance.bottom);
        word[29] = uint32_t(instance.bottom >> 32);
        word[30] = word[31] = 0;
      }
      return out;
    }

    // The tree's bounds from its boxes' new positions, keeping its shape
    // (TinyBVH refits triangles only). Children follow their parents.
    void refitBoxes(tinybvh::BVH &bvh, const std::vector<tinybvh::bvhvec4> &boxes) {
      for (int64_t i = int64_t(bvh.usedNodes) - 1; i >= 0; --i) {
        if (i == 1)
          continue; // unused: the root's sibling slot
        auto &node = bvh.bvhNode[i];
        if (node.isLeaf()) {
          tinybvh::bvhvec3 low(std::numeric_limits<float>::max()), high(std::numeric_limits<float>::lowest());
          for (uint32_t j = 0; j < node.triCount; ++j) {
            const uint32_t box = bvh.primIdx[node.leftFirst + j];
            low = tinybvh::tinybvh_min(low, tinybvh::bvhvec3(boxes[box * 2]));
            high = tinybvh::tinybvh_max(high, tinybvh::bvhvec3(boxes[box * 2 + 1]));
          }
          node.aabbMin = low;
          node.aabbMax = high;
        } else {
          const auto &left = bvh.bvhNode[node.leftFirst], &right = bvh.bvhNode[node.leftFirst + 1];
          node.aabbMin = tinybvh::tinybvh_min(left.aabbMin, right.aabbMin);
          node.aabbMax = tinybvh::tinybvh_max(left.aabbMax, right.aabbMax);
        }
      }
    }
  } // namespace

  Structure buildBottom(const Geometry *geometries, size_t count) {
    std::vector<tinybvh::bvhvec4> vertices;
    std::vector<uint32_t> source;
    gather(geometries, count, vertices, source);
    const auto triangles = uint32_t(source.size() / 2);
    if (!triangles)
      return empty();
    tinybvh::BVH bvh;
    bvh.Build(vertices.data(), triangles);
    return packBottom(bvh, vertices, source);
  }

  struct BottomBuilder::State {
    tinybvh::BVH bvh;
    std::vector<tinybvh::bvhvec4> vertices; // what bvh refers to
    std::vector<uint32_t> source, previousSource;
    uint64_t topology = 0;
    uint32_t refits = 0;
    bool built = false;
  };
  BottomBuilder::BottomBuilder() : m_state(std::make_unique<State>()) {}
  BottomBuilder::~BottomBuilder() = default;
  Structure BottomBuilder::update(const Geometry *geometries, size_t count, uint64_t topology, uint32_t refitLimit) {
    auto &s = *m_state;
    s.previousSource.swap(s.source);
    const auto *storage = s.vertices.data();
    const size_t stored = s.vertices.size();
    gather(geometries, count, s.vertices, s.source);
    const auto triangles = uint32_t(s.source.size() / 2);
    if (!triangles) {
      s.built = false;
      return empty();
    }
    // A refit keeps the tree and moves its bounds: for the same triangles
    // (the tree refers to the vertex storage, refilled in place) and, with a
    // limit, only so many times in a row, as the tree's shape ages.
    m_refitted = s.built && topology == s.topology && s.source == s.previousSource && s.vertices.data() == storage &&
                 s.vertices.size() == stored && (!refitLimit || s.refits < refitLimit);
    if (m_refitted) {
      s.bvh.Refit();
      ++s.refits;
    } else {
      s.bvh.Build(s.vertices.data(), triangles);
      s.refits = 0;
    }
    s.topology = topology;
    s.built = true;
    return packBottom(s.bvh, s.vertices, s.source);
  }

  Structure buildTop(const Instance *instances, size_t count) {
    std::vector<Kept> kept;
    std::vector<tinybvh::bvhvec4> boxes;
    collect(instances, count, kept, boxes);
    if (kept.empty())
      return empty();
    tinybvh::BVH bvh;
    bvh.BuildAABB(boxes.data(), uint32_t(kept.size()));
    return packTop(bvh, kept);
  }

  struct TopBuilder::State {
    tinybvh::BVH bvh;
    std::vector<Kept> kept;
    std::vector<tinybvh::bvhvec4> boxes;
    size_t count = 0, built = 0; // the instances given and kept at the last build
  };
  TopBuilder::TopBuilder() : m_state(std::make_unique<State>()) {}
  TopBuilder::~TopBuilder() = default;
  Structure TopBuilder::update(const Instance *instances, size_t count, bool refit) {
    auto &s = *m_state;
    collect(instances, count, s.kept, s.boxes);
    if (s.kept.empty()) {
      s.built = 0;
      return empty();
    }
    // A refit is for the instances of the last build in their order, moved.
    m_refitted = refit && s.built && count == s.count && s.kept.size() == s.built && s.bvh.hasOrderedNodes() &&
                 !s.bvh.mayHaveHoles();
    if (m_refitted)
      refitBoxes(s.bvh, s.boxes);
    else
      s.bvh.BuildAABB(s.boxes.data(), uint32_t(s.kept.size()));
    s.count = count;
    s.built = s.kept.size();
    return packTop(s.bvh, s.kept);
  }
} // namespace remix::trace::software
