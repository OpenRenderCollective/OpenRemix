#include "pathtrace/gpu.h"

namespace remix {
  using namespace trace::detail;

  // ORCA (orca.glsl): one hash table, cleared and refilled every frame by the
  // sparse pass before the opaque pass reads it. No history.
  void PathTracer::Gpu::configureOrca(Frame &frame, const TraceOptions &options) {
    auto &o = orca;
    frame.orcaTable = 0;
    std::fill_n(frame.orcaConfig, 4, 0u);
    std::fill_n(frame.orcaParams, 4, 0.f);
    if (options.orca && !o.pipelineFailed && !o.sparse) {
      try {
        createLightingPipelines({{lightingStage(trace::Stage::orcaSparse), layout, &o.sparse, false}});
      } catch (VkResult error) {
        // Still compiling in the background: retry on a later frame.
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        if (error != VK_NOT_READY)
          o.pipelineFailed = true;
      }
    }
    o.active = options.orca && o.sparse && options.bounces > 0;
    if (o.active && !o.table) {
      try {
        o.table = buffer(VkDeviceSize(Orca::capacity) * 40,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      } catch (VkResult error) {
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        o.active = false;
      }
    }
    if (!o.active)
      return;
    o.tile = std::clamp(options.orcaTile, 2u, 8u);
    frame.orcaTable = o.table->address;
    frame.orcaConfig[0] = Orca::capacity;
    frame.orcaConfig[1] = o.tile;
    frame.orcaConfig[2] = std::clamp(options.orcaLevels, 1u, 8u);
    frame.orcaConfig[3] = 1;
    frame.orcaParams[0] = std::max(options.orcaCellSize, .001f) / options.sceneScale;
    frame.orcaParams[1] = 4;
    frame.orcaParams[2] = float(std::max(options.orcaMinSamples, 1u));
    frame.orcaParams[3] = std::max(options.orcaMaxError, 0.f);
  }

  // After the G-buffer and SHARC (sparse path tails may query it), before any
  // pass reads the first bounce: clear the table and trace the sparse paths.
  void PathTracer::Gpu::recordOrca(VkCommandBuffer command, VkExtent2D extent) {
    auto &o = orca;
    if (!o.active)
      return;
    gpu::GpuScope scope(command, "ORCA sparse paths and cache", extent);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(command, o.table->handle, 0, VK_WHOLE_SIZE, 0);
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    bindSelected(command, selectLightingPipeline(o.sparse));
    const uint32_t tilesX = (extent.width + o.tile - 1) / o.tile, tilesY = (extent.height + o.tile - 1) / o.tile;
    vkCmdDispatch(command, (tilesX + 7) / 8, (tilesY + 7) / 8, 1);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  }
} // namespace remix
