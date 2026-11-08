#include "pathtrace/gpu.h"

namespace remix {
  using namespace trace::detail;

  // Presampled light pools (regir.glsl): two cell tables (this frame's
  // registrations, last frame's cells being presampled and looked up) and the
  // slot pool. Pools are redrawn every frame from the current light tree, so
  // moving lights and rebuilt trees need no invalidation.
  void PathTracer::Gpu::configureRegir(Frame &frame, const TraceOptions &options) {
    auto &r = regir;
    frame.regirRead = frame.regirWrite = frame.regirSlots = 0;
    std::fill_n(frame.regirConfig, 4, 0u);
    if (options.regir && !r.presample && !r.pipelineFailed) {
      try {
        createLightingPipelines({{trace::Stage::regirPresample, layout, &r.presample, false, false, false, false}});
      } catch (VkResult error) {
        // Still compiling in the background: retry on a later frame.
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        if (error != VK_NOT_READY)
          r.pipelineFailed = true;
      }
    }
    r.active = options.regir && r.presample && localIlluminationActive && lightBvh.nodes().size() > 1;
    if (r.active && !r.pool) {
      try {
        const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        r.cells[0] = buffer(VkDeviceSize(Regir::capacity) * 16, usage);
        r.cells[1] = buffer(VkDeviceSize(Regir::capacity) * 16, usage);
        r.pool = buffer(VkDeviceSize(Regir::capacity) * Regir::slots * 8, usage);
      } catch (VkResult error) {
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        r.cells[0].reset();
        r.cells[1].reset();
        r.pool.reset();
        r.active = false;
      }
    }
    if (!r.active) {
      r.valid = false;
      return;
    }
    r.writeIndex ^= 1;
    frame.regirWrite = r.cells[r.writeIndex]->address;
    frame.regirRead = r.valid ? r.cells[r.writeIndex ^ 1]->address : 0;
    frame.regirSlots = r.pool->address;
    frame.regirConfig[0] = Regir::capacity;
    frame.regirConfig[1] = Regir::slots;
    const float scale = options.sceneScale / std::max(options.regirCellSize, .01f);
    std::memcpy(&frame.regirConfig[2], &scale, 4);
    frame.regirConfig[3] = 1;
  }

  // Before any pass samples lights: clear this frame's registration table and
  // fill the pools of the cells registered last frame.
  void PathTracer::Gpu::recordRegir(VkCommandBuffer command) {
    auto &r = regir;
    if (!r.active)
      return;
    gpu::GpuScope scope(command, "Light cell presampling (ReGIR)");
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdFillBuffer(command, r.cells[r.writeIndex]->handle, 0, VK_WHOLE_SIZE, 0);
    if (r.valid) {
      barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_WRITE_BIT);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
      bindTrace(command, r.presample, layout, defaultFeatures);
      vkCmdDispatch(command, Regir::capacity * Regir::slots / 64, 1, 1);
    }
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    r.valid = true; // this frame registers the next frame's cells
  }
} // namespace remix
