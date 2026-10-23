#include "forward/gpu.h"
#include "platform/gpu_timers.h"

namespace remix::forward {
  void Renderer::Gpu::recordViewModels(VkCommandBuffer command) {
    const auto &frame = frames[slot];
    if (frame.viewModelCommands.empty()) return;
    gpu::GpuScope time(command, "Forward+ lit viewmodel overlay", extent);
    if (viewModelLocalMaps) {
      gpu::GpuScope shadowTime(command, "Forward+ viewmodel local self shadows",
                                options.localShadowResolution, options.localShadowResolution);
      viewModelLocalMaps->record(command, shadowSelection, [&](uint32_t slot, uint32_t face, bool) {
        const uint32_t cascade = slot * shadows::faces + face;
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, localShadowPipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                &frame.viewModelScene, 0, nullptr);
        for (const auto &draw : frame.viewModelCommands) {
          if (!intersectsClip(draw.worldBounds, frame.data.localShadows[cascade])) continue;
          vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                                  &draw.material, 0, nullptr);
          const uint32_t push[] = {draw.index, cascade};
          vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                             0, sizeof(push), push);
          const VkDeviceSize offset = 0;
          vkCmdBindVertexBuffers(command, 0, 1, &draw.geometry->vertices->handle, &offset);
          vkCmdBindIndexBuffer(command, draw.geometry->indices->handle, 0, VK_INDEX_TYPE_UINT32);
          vkCmdDrawIndexed(command, draw.count, 1, draw.source.firstIndex, draw.source.vertexOffset, 0);
        }
      });
    }
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
      VkPipeline activePipeline = VK_NULL_HANDLE;
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frame.viewModelScene, 0, nullptr);
      for (const auto &draw : frame.viewModelCommands) {
        if (!draw.source.rasterVisible || !intersectsClip(draw.worldBounds, frame.data.camera)) continue;
        // Per-draw ranges distinguish hidden close-up geometry from the world
        // receiver runs without changing overlay order or its isolated depth.
        const auto pipeline = viewModelEarlyPipeline && draw.unconditionalCoverage
            ? viewModelEarlyPipeline : viewModelPipeline;
        fragmentStatistics->begin(command, pipeline == viewModelEarlyPipeline
            ? FragmentStatistics::viewModelEarly : FragmentStatistics::viewModel);
        if (pipeline != activePipeline) {
          vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          activePipeline = pipeline;
        }
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &draw.material, 0, nullptr);
        const uint32_t push[] = {draw.index, 0};
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), push);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(command, 0, 1, &draw.geometry->vertices->handle, &offset);
        vkCmdBindIndexBuffer(command, draw.geometry->indices->handle, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command, draw.count, 1, draw.source.firstIndex, draw.source.vertexOffset, 0);
        fragmentStatistics->end(command);
      }
    });
  }
} // namespace remix::forward
