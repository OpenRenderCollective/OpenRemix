#include "pathtrace/gpu.h"

#include <cstring>

namespace remix {
  using namespace trace::detail;

  // remixapi_InstanceInfoCameraCullingEXT: the flags this frame's camera rays
  // apply, and for VISIBILITY the leaves the camera sees. The camera's cluster
  // is found here, once per frame; the shaders locate each candidate surface
  // point in the same partition (cameraUnseen in scene.glsl) and read its
  // leaf's bit. Nothing is uploaded or set while no traced instance asks.
  void PathTracer::Gpu::configureCameraCulling(VkCommandBuffer command, Frame &frame, const TraceOptions &options,
                                               const RasterRenderer &resources) {
    auto &c = cameraCulling;
    auto &s = c.statistics;
    const auto &partition = resources.visibilityPartition();
    // (The shader walks the nodes without bounds checks: closed partitions only.)
    const bool usable = partition && partition->closed && !partition->nodes.empty() && partition->clusterCount;
    const int32_t cluster = usable ? partition->cluster(frame.eye) : -1;
    // The previous traced frame's cluster, in the same partition.
    const int32_t previous = usable && c.lastVersion == partition->version ? c.lastCluster : cluster;
    c.lastVersion = usable ? partition->version : 0;
    c.lastCluster = cluster;
    const uint32_t requested = resources.cameraCull() & 3u;
    uint32_t active = options.cameraBackFaceCulling ? requested & 1u : 0u;
    const uint64_t updates = s.updates;
    s = {.structSize = sizeof(s)};
    s.updates = updates;
    s.requested = requested;
    s.partition = usable;
    s.outside = usable && (cluster < 0 || previous < 0);
    s.cameraCluster = cluster;
    s.previousCluster = previous;
    s.clusterCount = usable ? partition->clusterCount : 0;
    s.leafCount = usable ? uint32_t(partition->leafClusters.size()) : 0;
    s.visibleClusters = s.clusterCount;
    s.visibleLeaves = s.leafCount;
    auto retire = [&](std::unique_ptr<Buffer> &out) {
      if (out)
        retiredBuffers.push_back(std::move(out));
    };
    // Buffers are replaced, not rewritten: frames in flight keep theirs.
    auto place = [&](std::unique_ptr<Buffer> &out, const void *data, size_t size) {
      retire(out);
      if (deviceLocalScene)
        out = upload(command, data, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      else
        hostBuffer(out, data, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    };
    if (!(requested & 2u) || !options.cameraVisibilityCulling || !usable) {
      // Not asked for (any more): give the buffers back.
      if (!(requested & 2u) || !usable) {
        retire(c.nodes);
        retire(c.leaves);
        c.nodesVersion = c.leavesVersion = 0;
      }
    } else if (cluster >= 0 && previous >= 0) {
      if (c.nodesVersion != partition->version || !c.nodes || c.nodes->host == deviceLocalScene) {
        place(c.nodes, partition->nodes.data(), partition->nodes.size() * sizeof(partition->nodes[0]));
        c.nodesVersion = partition->version;
        c.leavesVersion = 0;
      }
      static_assert(sizeof(partition->nodes[0]) == 32);
      if (c.leavesVersion != partition->version || !c.leaves || c.leaves->host == deviceLocalScene ||
          c.leavesClusters[0] != cluster || c.leavesClusters[1] != previous) {
        std::vector<uint32_t> bits;
        const auto count = partition->visibleLeaves(cluster, previous, bits);
        place(c.leaves, bits.data(), bits.size() * sizeof(uint32_t));
        c.leavesVersion = partition->version;
        c.leavesClusters[0] = cluster;
        c.leavesClusters[1] = previous;
        c.visibleClusters = count.clusters;
        c.visibleLeaves = count.leaves;
        ++s.updates;
      }
      s.visibleClusters = c.visibleClusters;
      s.visibleLeaves = c.visibleLeaves;
      frame.cameraNodes = c.nodes->address;
      frame.cameraLeaves = c.leaves->address;
      active |= 2u;
    }
    s.active = active;
    frame.cameraCull[0] = active;
    // A camera ray is located 5 cm before the surface it meets.
    const float probe = .05f / options.sceneScale;
    std::memcpy(&frame.cameraCull[1], &probe, sizeof(probe));
  }
} // namespace remix
