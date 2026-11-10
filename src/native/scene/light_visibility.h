#pragma once
#include "scene/light_bvh.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace remix::scene {
  // A host's potentially-visible-set partition of space
  // (remixapi_SetVisibilityPartitionEXT): a binary space partition whose
  // leaves belong to clusters, and a conservative cluster-to-cluster
  // visibility matrix. Lights outside a shading point's potentially visible
  // clusters cannot illuminate it and are removed from light selection.
  struct VisibilityPartition {
    // std430 record shared with light_bvh.glsl. Point p is in front of the
    // plane when dot(normal, p) - distance >= 0. Children >= 0 are nodes,
    // < 0 are leaves (-1 - leaf index).
    struct Node {
      float plane[4];
      int32_t children[2];
      int32_t reserved[2];
    };
    std::vector<Node> nodes;
    std::vector<int32_t> leafClusters; // -1: no cluster (solid or outside)
    uint32_t clusterCount = 0, rowBytes = 0;
    std::vector<uint8_t> visibility; // clusterCount rows of rowBytes
    uint64_t version = 0;

    // Every child names a node or a leaf that exists (checked when set), so a
    // walk from the root needs no bounds checks.
    bool closed = false;
    enum class SegmentState { Unknown, Clear, Solid };
    struct SegmentResult { SegmentState state = SegmentState::Unknown; double fraction = 0; }; // entry in [0,1], clear=1
    // First solid/unplayable leaf along a segment in a closed partition.
    // Unknown covers absent/malformed partitions and exhausted traversal budgets.
    SegmentResult firstSolid(const float start[3], const float end[3], uint32_t budget = 4096) const;
    // Cluster of a point, or -1.
    int32_t cluster(const float point[3]) const;
    bool visible(int32_t from, int32_t to) const {
      return (visibility[size_t(from) * rowBytes + uint32_t(to) / 8] >> (uint32_t(to) % 8)) & 1;
    }
    // The leaves potentially visible from cluster a or cluster b (both valid):
    // a bit per leaf, in 32-bit words; leaves without a cluster are set.
    // Returns the clusters and the leaves counted as visible.
    struct VisibleCount {
      uint32_t clusters = 0, leaves = 0;
    };
    VisibleCount visibleLeaves(int32_t a, int32_t b, std::vector<uint32_t> &bits) const;
  };
  static_assert(sizeof(VisibilityPartition::Node) == 32);

  // Per (cluster, light-tree node): whether the node holds a light potentially
  // visible from the cluster. The table covers the tree's static nodes
  // (LightBvh::staticNodes); the dynamic part behind them is visible from
  // everywhere, so lights that come, go or belong to moving instances never
  // change it. Static lights without a cluster, and static lights that moved
  // since the tree was built, are visible from everywhere as well.
  class LightVisibility {
  public:
    // Returns whether the table changed.
    // probe: world distance around a light whose center lies outside every
    // cluster (a fixture embedded in a wall) at which its clusters are sought.
    bool update(const std::shared_ptr<const VisibilityPartition> &partition, const LightBvh &tree, bool enabled,
                float probe);
    const std::shared_ptr<const std::vector<uint32_t>> &table() const { return m_table; }
    const std::shared_ptr<const VisibilityPartition> &partition() const { return m_partition; }
    uint32_t rowWords() const { return m_rowWords; }
    // Nodes from this index on are not in the table: always visible.
    uint32_t tableNodes() const { return m_tableNodes; }
    uint64_t version() const { return m_version; }
    // Statistics: lights with clusters, lights visible from everywhere.
    uint32_t clustered = 0, unrestricted = 0;
    // Tables made, and how long the last one took.
    uint64_t tableBuilds = 0;
    double tableMilliseconds = 0, tableTotalMilliseconds = 0;

  private:
    // Per cluster with a light: the clusters it is potentially visible from
    // (a column of the partition's matrix), gathered once per partition.
    struct Columns {
      uint64_t version = ~0ull;
      std::vector<int32_t> slots; // per cluster: its column, or -1
      std::vector<uint64_t> bits;
    };
    std::shared_ptr<Columns> m_columns; // shared by copies; used by update() only
    std::shared_ptr<const VisibilityPartition> m_partition;
    std::shared_ptr<const std::vector<uint32_t>> m_table;
    uint64_t m_partitionVersion = ~0ull, m_treeBuilds = ~0ull, m_version = 0;
    uint32_t m_rowWords = 0, m_tableNodes = 0;
    // Per static emitter: up to kClusters clusters it may occupy (-1: unused); all -1
    // with m_unrestricted set: visible from everywhere.
    static constexpr uint32_t kClusters = 4;
    std::vector<int32_t> m_lightClusters;
    std::vector<uint8_t> m_unrestricted;
    void locate(const VisibilityPartition &partition, size_t light, const float *position, float probe);
    void build(const VisibilityPartition &partition, const LightBvh &tree);
    std::vector<float> m_positions;       // per static emitter, since the tree was built
  };
} // namespace remix::scene
