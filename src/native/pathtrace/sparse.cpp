#include "pathtrace/gpu.h"
#include "pathtrace/sparse_policy.h"

namespace remix {
  using namespace trace::detail;

  // Sparse rendering (see TraceOptions::sparseRendering). The opaque lighting
  // pass is the frame's largest cost and scales with the pixels it lights. A
  // blue-noise threshold selects this frame's pixels (pathtrace.comp,
  // sparseSelected); the compaction pass lists them, each 16x16 tile's as one
  // run in Z order, so the lighting launch over the list keeps neighbouring
  // pixels in one group. The pixels not selected get a launch of the same
  // shader without lighting: guides, emission and sky are exact everywhere.
  void PathTracer::Gpu::configureSparse(VkCommandBuffer command, Frame &frame, const TraceOptions &options,
                                        VkExtent2D extent) {
    auto &s = sparse;
    frame.sparseList = frame.sparseCount = 0;
    std::fill_n(frame.sparseConfig, 4, 0u);
    s.active = s.launched = false;
    s.rate = 1;
    const float rate = trace::sparseSamplingRate(options, uint64_t(extent.width) * extent.height);
    s.inactiveReason = !options.sparseRendering           ? "not requested"
                       : !rrActive()                      ? "needs DLSS Ray Reconstruction"
                       : restir.active                    ? "ReSTIR is active"
                       : splitLightingActive && postSplit ? "separate local-lighting pass is active"
                       : indirectPass.active              ? "separate indirect pass is active"
                       : pbrReuse.active                  ? "local-light reuse is active"
                       : options.accumulate               ? "accumulation is on"
                       : frame.splitLocal[3] || frame.sun[3] != 0 || options.sharcDebug || options.debugView
                           ? "a debug view is on"
                       : rate > options.sparseMaxRate ? "the rate at this render resolution exceeds sparseMaxRate"
                       : extent.width > 65535 || extent.height > 65535 ? "render resolution too large"
                       : s.pipelineFailed                              ? "its passes failed to compile"
                                                                       : nullptr;
    if (s.inactiveReason)
      return;
    if (!s.compact || !s.arguments) {
      try {
        createLightingPipelines({{trace::Stage::sparseCompact, layout, &s.compact, false, false, false, false},
                                 {trace::Stage::sparseArgs, layout, &s.arguments, false, false, false, false}});
      } catch (VkResult error) {
        // Still compiling in the background: retry on a later frame.
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        if (error != VK_NOT_READY)
          s.pipelineFailed = true;
      }
      if (!s.compact || !s.arguments) {
        s.inactiveReason = s.pipelineFailed ? "its passes failed to compile" : "its passes are compiling";
        return;
      }
    }
    try {
      const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 4;
      if (!s.list || s.list->size < bytes) {
        auto next = buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (s.list)
          retiredBuffers.push_back(std::move(s.list));
        s.list = std::move(next);
      }
      if (!s.count) {
        s.count = buffer(32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        // The accumulator starts at zero; the arguments pass resets it after.
        vkCmdFillBuffer(command, s.count->handle, 0, VK_WHOLE_SIZE, 0);
        barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
      }
    } catch (VkResult error) {
      if (error == VK_ERROR_DEVICE_LOST)
        throw;
      s.list.reset();
      s.count.reset();
      s.inactiveReason = "its buffers could not be allocated";
      return;
    }
    const uint32_t levels = trace::sparseLevels(rate);
    s.rate = float(levels) / 256;
    s.active = true;
    frame.sparseList = s.list->address;
    frame.sparseCount = s.count->address;
    frame.sparseConfig[0] = 1;
    frame.sparseConfig[1] = levels;
  }

  // Before the opaque pass: list the selected pixels and publish their count
  // as the lighting launch's indirect dispatch.
  void PathTracer::Gpu::recordSparse(VkCommandBuffer command, VkExtent2D extent) {
    auto &s = sparse;
    if (!s.active)
      return;
    gpu::GpuScope scope(command, "Sparse rendering: select and list pixels", extent);
    // The list and count are queue-ordered across frame slots: last frame's
    // launch has read them before this frame's passes overwrite them.
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    bindTrace(command, s.compact, layout, defaultFeatures);
    vkCmdDispatch(command, (extent.width + 15) / 16, (extent.height + 15) / 16, 1);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    bindTrace(command, s.arguments, layout, defaultFeatures);
    vkCmdDispatch(command, 1, 1, 1);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
  }
} // namespace remix
