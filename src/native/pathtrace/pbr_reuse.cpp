#include "pathtrace/gpu.h"

namespace remix {
  using namespace trace::detail;

  void PathTracer::Gpu::configurePbrReuse(VkCommandBuffer command, Frame &frame, const TraceOptions &options,
                                          uint64_t signature, bool stable) {
    if (!options.pbrLocalReuse)
      pbrReuse.failed = false;
    pbrReuse.active = !pbrReuse.failed && options.pbrLocalReuse && options.localDirectClamp == 0 &&
                      options.indirectClamp == 0 && denoiser && postReblur && !options.accumulate &&
                      options.samples == 1 && options.bounces && frame.localConfig[0];
    if (!pbrReuse.active) {
      pbrReuse.valid = false;
      for (auto &buffer : pbrReuse.history)
        if (buffer)
          retiredBuffers.push_back(std::move(buffer));
      return;
    }
    const VkDeviceSize bytes = 16 + VkDeviceSize(extent.width) * extent.height * 64;
    bool allocated = false;
    try {
      for (auto &history : pbrReuse.history) {
        if (!history || history->size < bytes) {
          if (history)
            retiredBuffers.push_back(std::move(history));
          history = buffer(bytes,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           false);
          allocated = true;
        }
      }
    } catch (VkResult error) {
      if (error == VK_ERROR_DEVICE_LOST)
        throw;
      pbrReuse.failed = true;
      pbrReuse.active = pbrReuse.valid = false;
      for (auto &history : pbrReuse.history)
        if (history)
          retiredBuffers.push_back(std::move(history));
      std::fprintf(stderr, "PBR local reuse unavailable (%d); tracing fresh local lighting\n", int(error));
      return;
    }
    if (allocated)
      pbrReuse.valid = false;
    frame.reuseWrite = pbrReuse.history[pbrReuse.writeIndex]->address;
    frame.reuseRead = pbrReuse.history[pbrReuse.writeIndex ^ 1]->address;
    frame.reuseConfig[0] = pbrReuse.valid && stable && pbrReuse.signature == signature && !temporalFrame.flags[0];
    frame.reuseConfig[1] = pbrReuse.diagnostics != nullptr;
    pbrReuse.signature = signature;
    // The two histories are queue-ordered across frame slots. Cover both earlier
    // reads and writes before recycling the current write buffer's counters.
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdFillBuffer(command, pbrReuse.history[pbrReuse.writeIndex]->handle, 0, 16, 0);
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (pbrReuse.diagnostics && !pbrReuse.readback)
      pbrReuse.readback = buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  }

  void PathTracer::Gpu::finishPbrReuse(VkCommandBuffer command) {
    if (!pbrReuse.active || !pbrReuse.diagnostics)
      return;
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy copy{.size = 16};
    vkCmdCopyBuffer(command, pbrReuse.history[pbrReuse.writeIndex]->handle, pbrReuse.readback->handle, 1, &copy);
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            VK_ACCESS_HOST_READ_BIT);
    pbrReuse.pending = true;
  }

  void PathTracer::Gpu::completePbrReuse() {
    if (!pbrReuse.pending)
      return;
    void *mapped{};
    check(vkMapMemory(device, pbrReuse.readback->memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    VkMappedMemoryRange range{
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = pbrReuse.readback->memory, .size = VK_WHOLE_SIZE};
    const auto result = vkInvalidateMappedMemoryRanges(device, 1, &range);
    uint32_t counts[4] = {};
    if (result == VK_SUCCESS)
      std::memcpy(counts, mapped, sizeof(counts));
    vkUnmapMemory(device, pbrReuse.readback->memory);
    check(result);
    if (auto *file = std::fopen(pbrReuse.diagnostics, "a")) {
      if (std::ftell(file) == 0)
        std::fprintf(file, "attempts,hits,geometry_rejects,view_rejects\n");
      std::fprintf(file, "%u,%u,%u,%u\n", counts[0], counts[1], counts[2], counts[3]);
      std::fclose(file);
    }
    pbrReuse.pending = false;
  }
} // namespace remix
