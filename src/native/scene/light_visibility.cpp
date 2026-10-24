#include "scene/light_visibility.h"

#include <algorithm>
#include <chrono>

namespace remix::scene {
  int32_t VisibilityPartition::cluster(const float p[3]) const {
    if (nodes.empty())
      return -1;
    int32_t index = 0;
    for (uint32_t guard = 0; index >= 0 && guard < 1024; ++guard) {
      if (size_t(index) >= nodes.size())
        return -1;
      const auto &n = nodes[index];
      index = n.plane[0] * p[0] + n.plane[1] * p[1] + n.plane[2] * p[2] - n.plane[3] >= 0 ? n.children[0]
                                                                                          : n.children[1];
    }
    if (index >= 0)
      return -1;
    const size_t leaf = size_t(-1 - int64_t(index));
    if (leaf >= leafClusters.size())
      return -1;
    const int32_t c = leafClusters[leaf];
    return c >= 0 && uint32_t(c) < clusterCount ? c : -1;
  }

  VisibilityPartition::VisibleCount VisibilityPartition::visibleLeaves(int32_t a, int32_t b,
                                                                       std::vector<uint32_t> &bits) const {
    VisibleCount count;
    bits.assign(std::max<size_t>(1, (leafClusters.size() + 31) / 32), 0u);
    for (uint32_t c = 0; c < clusterCount; ++c)
      count.clusters += visible(a, int32_t(c)) || visible(b, int32_t(c));
    for (size_t leaf = 0; leaf < leafClusters.size(); ++leaf) {
      const int32_t c = leafClusters[leaf];
      if (c >= 0 && uint32_t(c) < clusterCount && !visible(a, c) && !visible(b, c))
        continue;
      bits[leaf / 32] |= 1u << (leaf % 32);
      ++count.leaves;
    }
    return count;
  }

  void LightVisibility::locate(const VisibilityPartition &partition, size_t light, const float *p, float probe) {
    int32_t *clusters = &m_lightClusters[light * kClusters];
    std::fill_n(clusters, kClusters, -1);
    m_unrestricted[light] = 0;
    if (const int32_t c = partition.cluster(p); c >= 0) {
      clusters[0] = c;
      return;
    }
    // Center in solid space: the clusters around it, found along the axes.
    uint32_t found = 0;
    for (int axis = 0; axis < 3; ++axis)
      for (float sign : {-1.f, 1.f}) {
        float q[3] = {p[0], p[1], p[2]};
        q[axis] += sign * probe;
        const int32_t c = partition.cluster(q);
        if (c < 0 || std::find(clusters, clusters + found, c) != clusters + found)
          continue;
        if (found == kClusters) {
          m_unrestricted[light] = 1;
          return;
        }
        clusters[found++] = c;
      }
    if (!found)
      m_unrestricted[light] = 1;
  }

  namespace {
    // In-place transpose of a 64x64 bit matrix: row r, bit c <-> row c, bit r.
    void transpose(uint64_t rows[64]) {
      uint64_t mask = 0x00000000ffffffffull;
      for (unsigned j = 32; j; j >>= 1, mask ^= mask << j)
        for (unsigned k = 0; k < 64; k = ((k | j) + 1) & ~j) {
          const uint64_t t = ((rows[k] >> j) ^ rows[k | j]) & mask;
          rows[k] ^= t << j;
          rows[k | j] ^= t;
        }
    }
  } // namespace

  bool LightVisibility::update(const std::shared_ptr<const VisibilityPartition> &partition, const LightBvh &tree,
                               bool enabled, float probe) {
    const auto &nodes = tree.nodes();
    const auto &emitters = tree.emitters();
    if (!enabled || !partition || partition->clusterCount == 0 || nodes.empty()) {
      const bool changed = m_table != nullptr;
      m_table.reset();
      m_partition.reset();
      m_partitionVersion = m_treeBuilds = ~0ull;
      clustered = unrestricted = 0;
      if (changed)
        ++m_version;
      return changed;
    }
    // A rebuilt tree or a new partition assigns every static light afresh;
    // only those that move afterwards become unrestricted. A light that left
    // keeps its slot and its bits: a superset of the visible lights is safe.
    const size_t statics = tree.staticEmitters();
    bool rebuild = partition->version != m_partitionVersion || tree.builds != m_treeBuilds ||
                   m_unrestricted.size() != statics;
    if (rebuild) {
      m_partition = partition;
      m_partitionVersion = partition->version;
      m_treeBuilds = tree.builds;
      m_lightClusters.assign(statics * kClusters, -1);
      m_unrestricted.assign(statics, 0);
      m_positions.assign(statics * 3, 0);
      for (size_t i = 0; i < statics; ++i) {
        const float *p = emitters[i].positionRadius;
        std::copy_n(p, 3, &m_positions[i * 3]);
        if (tree.staticLights + tree.retiredSlots) // not the unlit slot of an empty static part
          locate(*partition, i, p, probe + emitters[i].positionRadius[3]);
      }
    } else {
      for (size_t i = 0; i < statics; ++i) {
        const float *p = emitters[i].positionRadius;
        float *last = &m_positions[i * 3];
        if (!m_unrestricted[i] && (p[0] != last[0] || p[1] != last[1] || p[2] != last[2])) {
          m_unrestricted[i] = 1;
          rebuild = true;
        }
        std::copy_n(p, 3, last);
      }
    }
    // Statistics: the dynamic part's lights are visible from everywhere.
    clustered = 0;
    unrestricted = tree.dynamicLights;
    if (tree.staticLights + tree.retiredSlots)
      for (uint8_t u : m_unrestricted)
        ++(u ? unrestricted : clustered);
    if (!rebuild)
      return false;
    const auto start = std::chrono::steady_clock::now();
    build(*partition, tree);
    tableMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    tableTotalMilliseconds += tableMilliseconds;
    ++tableBuilds;
    ++m_version;
    return true;
  }

  // Rows of node bits, one per cluster. Made per node first (a node's set of
  // clusters is the union of its children's: word operations), then turned
  // into rows 64x64 bits at a time.
  void LightVisibility::build(const VisibilityPartition &partition, const LightBvh &tree) {
    const auto &nodes = tree.nodes();
    const uint32_t clusters = partition.clusterCount, statics = tree.staticEmitters();
    const uint32_t count = std::min<uint32_t>(tree.staticNodes(), uint32_t(nodes.size()));
    const size_t words = (size_t(clusters) + 63) / 64;
    if (!m_columns || m_columns->version != partition.version) {
      m_columns = std::make_shared<Columns>();
      m_columns->version = partition.version;
      m_columns->slots.assign(clusters, -1);
    }
    auto &columns = *m_columns;
    const auto column = [&](int32_t cluster) {
      int32_t &slot = columns.slots[cluster];
      if (slot < 0) {
        slot = int32_t(columns.bits.size() / words);
        columns.bits.resize(columns.bits.size() + words, 0);
        uint64_t *bits = columns.bits.data() + size_t(slot) * words;
        for (uint32_t from = 0; from < clusters; ++from)
          bits[from / 64] |= uint64_t(partition.visible(int32_t(from), cluster)) << (from % 64);
      }
      return size_t(slot) * words;
    };
    // Gather first: the columns' storage must not move while it is read.
    for (uint32_t light = 0; light < statics && light < m_unrestricted.size(); ++light)
      for (uint32_t k = 0; k < kClusters && m_lightClusters[light * kClusters + k] >= 0; ++k)
        column(m_lightClusters[light * kClusters + k]);
    std::vector<uint64_t> sets(size_t(count) * words, 0);
    // Children always follow their parent in node order.
    for (size_t n = count; n-- > 0;) {
      uint64_t *set = sets.data() + n * words;
      const auto &links = nodes[n].links;
      const auto merge = [&](const uint64_t *from) {
        for (size_t w = 0; w < words; ++w)
          set[w] |= from[w];
      };
      if (links[3] <= 1) {
        // A leaf's emitters (LightBvh: consecutive, links[1] of them).
        for (size_t light = links[2]; light < size_t(links[2]) + std::max(links[1], 1u); ++light)
          if (light >= statics || light >= m_unrestricted.size() || m_unrestricted[light]) {
            std::fill_n(set, words, ~0ull); // dynamic part, or visible from everywhere
            break;
          } else {
            for (uint32_t k = 0; k < kClusters && m_lightClusters[light * kClusters + k] >= 0; ++k)
              merge(columns.bits.data() + column(m_lightClusters[light * kClusters + k]));
          }
      } else {
        for (unsigned child = 0; child < 2; ++child)
          if (links[child] >= count)
            std::fill_n(set, words, ~0ull); // the dynamic part's nodes
          else
            merge(sets.data() + size_t(links[child]) * words);
      }
    }
    m_tableNodes = count;
    m_rowWords = (count + 31) / 32;
    auto table = std::make_shared<std::vector<uint32_t>>(size_t(clusters) * m_rowWords, 0u);
    uint64_t block[64];
    for (size_t first = 0; first < count; first += 64)
      for (size_t w = 0; w < words; ++w) {
        for (size_t i = 0; i < 64; ++i)
          block[i] = first + i < count ? sets[(first + i) * words + w] : 0;
        transpose(block);
        for (size_t i = 0; i < 64 && w * 64 + i < clusters; ++i) {
          uint32_t *row = table->data() + (w * 64 + i) * m_rowWords + first / 32;
          row[0] = uint32_t(block[i]);
          if (first / 32 + 1 < m_rowWords)
            row[1] = uint32_t(block[i] >> 32);
        }
      }
    m_table = std::move(table);
  }
} // namespace remix::scene
