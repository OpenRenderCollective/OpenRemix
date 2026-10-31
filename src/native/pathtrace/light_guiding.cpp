#include "pathtrace/gpu.h"

namespace remix {
using namespace trace::detail;

void PathTracer::Gpu::configureLightGuiding(Frame &frame, const TraceOptions &options) {
  auto &g = lightGuiding;
  const bool split = frame.splitLocal[0] != 0;
  const auto &tree = split ? emissiveBvh : lightBvh;
  g.stats.requested = options.localGuiding;
  // Diagnostic: local estimator of this frame (bit 0 RIS, bit 1 sphere-only set).
  g.stats.reserved = uint32_t(localResamplingActive) | (frame.localConfig[3] & 0x40000000u ? 2u : 0u);
  g.stats.influenceRange = options.localRange;
  const float cellUnits = options.localGuideCellSize / options.sceneScale;
  g.stats.cellSize = cellUnits;
  localGuidingActive = false;
  g.stats.active = 0;
  if (g.pipelineFailed || !options.localGuiding || (split ? frame.splitLocal[2] : frame.localConfig[0]) < 2 || tree.nodes().size() >= (1u << 20)) {
    g.valid = false;
    g.stats.sampledQueries = g.stats.sampledGuided = g.stats.sampledVisible = 0;
    return;
  }
  if (!g.attempted) {
    g.attempted = true;
    try {
      const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      const VkDeviceSize bank = (LightGuiding::capacity + 1) * 16 + LightGuiding::remapCapacity * 4;
      auto first = buffer(bank, usage);
      auto second = buffer(bank, usage);
      auto readback = buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
      g.history[0] = std::move(first);
      g.history[1] = std::move(second);
      g.readback = std::move(readback);
      g.stats.capacity = LightGuiding::capacity;
      g.stats.memoryBytes = g.history[0]->size + g.history[1]->size + g.readback->size;
    } catch (VkResult error) {
      if (error == VK_ERROR_DEVICE_LOST)
        throw;
      std::strncpy(g.stats.unavailableReason, "Visibility history allocation failed; using LightBVH sampling",
                   sizeof(g.stats.unavailableReason) - 1);
    } catch (const std::exception &) {
      std::strncpy(g.stats.unavailableReason, "Visibility history initialization failed; using LightBVH sampling",
                   sizeof(g.stats.unavailableReason) - 1);
    }
  }
  if (!g.history[0])
    return;
  const bool resampled = split || frame.localScale[2] > 1;
  // The guide stores light-tree leaf indices and learned visibility. Neither
  // depends on light intensity: flickering or animated lights keep it (it is
  // only a proposal; every sample is still shadow-tested and fully weighted).
  // A rebuilt tree (new leaf order) carries it over through its leaf remap
  // (the read bank's leaves translated this frame, light_guiding.glsl
  // guideLeaf); anything else invalidates it.
  g.pendingRemap.clear();
  const std::vector<uint32_t> *remap = g.generation != tree.builds ? tree.leafRemap(g.generation) : nullptr;
  if (history.reset() || (g.generation != tree.builds && (!remap || remap->size() > LightGuiding::remapCapacity)) ||
      g.range != options.localRange || g.cellSize != cellUnits || g.resampled != resampled || g.split != split) {
    g.valid = false;
    ++g.stats.historyResets;
  } else if (remap && g.valid)
    g.pendingRemap = *remap;
  g.generation = tree.builds;
  g.split = split;
  g.range = options.localRange;
  g.cellSize = cellUnits;
  g.resampled = resampled;
  frame.lightGuideRead = g.valid ? g.history[g.writeIndex ^ 1]->address : 0;
  frame.lightGuideWrite = g.history[g.writeIndex]->address;
  frame.lightGuideConfig[0] = LightGuiding::capacity;
  frame.lightGuideConfig[1] = uint32_t(tree.nodes().size());
  frame.lightGuideConfig[3] = uint32_t(g.pendingRemap.size());
  const float scale = 1.f / cellUnits;
  std::memcpy(&frame.lightGuideConfig[2], &scale, 4);
  g.stats.active = 1;
  ++g.stats.renderedFrames;
  localGuidingActive = true;
}

void PathTracer::Gpu::beginLightGuiding(VkCommandBuffer command) {
  if (!localGuidingActive)
    return;
  // The read bank is immutable for the whole frame. Clear only the next bank,
  // after the prior submission's reads/writes, before any training atomics.
  barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  vkCmdFillBuffer(command, lightGuiding.history[lightGuiding.writeIndex]->handle, 0, VK_WHOLE_SIZE, 0);
  auto &remap = lightGuiding.pendingRemap;
  if (!remap.empty()) {
    const VkDeviceSize bytes = remap.size() * sizeof(uint32_t);
    const VkBufferCopy copy{.srcOffset = stage(remap.data(), bytes),
                            .dstOffset = (LightGuiding::capacity + 1) * 16,
                            .size = bytes};
    vkCmdCopyBuffer(command, staging.buffer->handle, lightGuiding.history[lightGuiding.writeIndex ^ 1]->handle, 1,
                    &copy);
  }
  barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void PathTracer::Gpu::finishLightGuiding(VkCommandBuffer command) {
  if (!localGuidingActive)
    return;
  auto &g = lightGuiding;
  barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
  const VkBufferCopy copy{LightGuiding::capacity * 16, 0, 16};
  vkCmdCopyBuffer(command, g.history[g.writeIndex]->handle, g.readback->handle, 1, &copy);
  barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
  g.pending = true;
}

void PathTracer::Gpu::completeLightGuiding() {
  auto &g = lightGuiding;
  if (!g.pending)
    return;
  void *mapped = nullptr;
  check(vkMapMemory(device, g.readback->memory, 0, VK_WHOLE_SIZE, 0, &mapped));
  const VkMappedMemoryRange range{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                 .memory = g.readback->memory, .size = VK_WHOLE_SIZE};
  const auto result = vkInvalidateMappedMemoryRanges(device, 1, &range);
  uint32_t counters[4] = {};
  if (result == VK_SUCCESS)
    std::memcpy(counters, mapped, sizeof(counters));
  vkUnmapMemory(device, g.readback->memory);
  check(result);
  g.stats.sampledQueries = counters[0];
  g.stats.sampledGuided = counters[1];
  g.stats.sampledVisible = counters[2];
  g.pending = false;
}
} // namespace remix
