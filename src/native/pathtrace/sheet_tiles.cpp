#include "pathtrace/block_costs.h"
#include "pathtrace/gpu.h"

namespace remix {
  using namespace trace::detail;

  // Sheet tiles (TraceOptions::sheetTiles). The sheet lighting pass and the
  // single-sheet composition pass are among the largest stages, launched over
  // the screen, while most pixels have no translucent sheet in front: those
  // only take the composition's last steps (media, depth, the result), at the
  // occupancy of a stage whose registers the sheets' lighting and walk set.
  // A classification pass after the opaque pass sorts the 8x8 tiles by
  // whether any pixel has a sheet (the opaque pass's count); the sheet passes
  // (lighting, composition, the multi-sheet pass) launch over the tiles with
  // one, and the plain composition, the composition stage built without the
  // sheets' code, over the rest. Every pixel is composed by exactly the steps
  // it took before: the image is the same.
  void PathTracer::Gpu::configureSheetTiles(Frame &frame, const TraceOptions &options, VkExtent2D extent,
                                            bool temporal) {
    auto &s = sheetTiles;
    frame.sheetTiles = 0;
    s.active = false;
    // The timings dump's line, however this returns.
    struct Publish {
      const SheetTilesState &s;
      bool requested;
      ~Publish() { trace::g_sheetTileState = {requested, s.active, s.active ? "" : s.inactiveReason, s.error}; }
    } publish{s, options.sheetTiles};
    s.inactiveReason = !options.sheetTiles                          ? "not requested"
                       : extent.width > 8 * 65535 || extent.height > 8 * 65535 ? "render resolution too large"
                       : s.pipelineFailed                           ? "its passes failed to compile"
                                                                    : nullptr;
    if (s.inactiveReason)
      return;
    VkPipeline &plain = temporal ? s.plainTemporal : s.plain;
    if (!s.classify || !plain) {
      try {
        createLightingPipelines(
            {{trace::Stage::sheetTiles, layout, &s.classify, false, false, false, false},
             {temporal ? trace::Stage::plainCompositionTemporal : trace::Stage::plainComposition,
              temporal ? temporalLayout : layout, &plain, false, false, false, false}});
      } catch (VkResult error) {
        // Still compiling in the background: retry on a later frame.
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        if (error != VK_NOT_READY) {
          s.pipelineFailed = true;
          s.error = int(error);
        }
      }
      if (!s.classify || !plain) {
        s.inactiveReason = s.pipelineFailed ? "its passes failed to compile" : "its passes are compiling";
        return;
      }
    }
    try {
      const VkDeviceSize tiles = VkDeviceSize((extent.width + 7) / 8) * ((extent.height + 7) / 8);
      const VkDeviceSize bytes = 32 + 4 * tiles;
      if (!s.buffer || s.buffer->size < bytes) {
        auto next = buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (s.buffer)
          retiredBuffers.push_back(std::move(s.buffer));
        s.buffer = std::move(next);
      }
    } catch (VkResult error) {
      if (error == VK_ERROR_DEVICE_LOST)
        throw;
      s.buffer.reset();
      s.inactiveReason = "its buffer could not be allocated";
      return;
    }
    s.active = true;
    frame.sheetTiles = s.buffer->address;
  }

  // After the opaque pass (the sheet counts) and before the sheet lighting
  // pass: the two lists and their dispatches. Binds the tracer's set with the
  // base layout; the caller binds its own again.
  void PathTracer::Gpu::recordSheetTiles(VkCommandBuffer command, VkExtent2D extent) {
    auto &s = sheetTiles;
    if (!s.active)
      return;
    gpu::GpuScope scope(command, "Sheet tiles: classify", extent);
    // Last frame's launches have read the dispatches before they are reset.
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    const uint32_t dispatches[8] = {0, 1, 1, 0, 0, 1, 1, 0};
    vkCmdUpdateBuffer(command, s.buffer->handle, 0, sizeof(dispatches), dispatches);
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    bindTrace(command, s.classify, layout, defaultFeatures);
    vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
  }
} // namespace remix
