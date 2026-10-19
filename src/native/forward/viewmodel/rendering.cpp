#include "forward/gpu.h"
#include "platform/gpu_timers.h"

namespace remix::forward {
  void Renderer::Gpu::recordViewModels(VkCommandBuffer command) {
    const auto &frame = frames[slot];
    if (frame.viewModelCommands.empty()) return;
    gpu::GpuScope time(command, "Forward+ lit viewmodel overlay", extent);
    if (frame.data.target[3]) {
      gpu::GpuScope shadowTime(command, "Forward+ viewmodel sun shadow",
                                options.shadowResolution, options.shadowResolution);
      viewModelSun->record(command, [&] {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                &frame.viewModelScene, 0, nullptr);
        auto casters = [&](const std::vector<Command> &commands, bool skipCaptured) {
          for (const auto &draw : commands) {
            if ((skipCaptured && draw.captured) ||
                !intersectsClip(draw.worldBounds, frame.viewModelProjection)) continue;
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                                    &draw.material, 0, nullptr);
            const uint32_t push[] = {draw.index, 0};
            vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), push);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command, 0, 1, &draw.geometry->vertices->handle, &offset);
            vkCmdBindIndexBuffer(command, draw.geometry->indices->handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command, draw.count, 1, draw.source.firstIndex, draw.source.vertexOffset, 0);
          }
        };
        casters(frame.commands, true);
        casters(frame.viewModelCasters, false);
        casters(frame.viewModelCommands, false);
      });
    }
    viewModelTarget->record(command, [&] {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, viewModelPipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frame.viewModelScene, 0, nullptr);
      for (const auto &draw : frame.viewModelCommands) {
        if (!draw.source.rasterVisible || !intersectsClip(draw.worldBounds, frame.data.camera)) continue;
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &draw.material, 0, nullptr);
        const uint32_t push[] = {draw.index, 0};
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), push);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(command, 0, 1, &draw.geometry->vertices->handle, &offset);
        vkCmdBindIndexBuffer(command, draw.geometry->indices->handle, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command, draw.count, 1, draw.source.firstIndex, draw.source.vertexOffset, 0);
      }
    });
  }
} // namespace remix::forward
