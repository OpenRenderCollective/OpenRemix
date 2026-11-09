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
        const ClipFrustum casterFrustum(frame.data.localShadows[cascade]);
        bool hasDraws = false;
        DrawBindings bindings;
        for (const auto &draw : frame.viewModelCommands) {
          if (!casterFrustum.intersects(draw.worldBounds)) continue;
          if (!hasDraws) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, localShadowPipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                    &frame.viewModelScene, 0, nullptr);
            hasDraws = true;
          }
          submitShadowCaster(command, draw, cascade, bindings);
          ++statistics.drawCalls.viewModelLocalShadow;
        }
      });
    }
    if (frame.data.target[3]) {
      const ClipFrustum casterFrustum(frame.viewModelProjection);
      gpu::GpuScope shadowTime(command, "Forward+ viewmodel sun shadow",
                                options.shadowResolution, options.shadowResolution);
      viewModelSun->record(command, frame.viewModelStaticSunDirty, [&](uint32_t layer) {
        bool hasDraws = false;
        DrawBindings bindings;
        auto casters = [&](const std::vector<Command> &commands, bool skipCaptured) {
          for (const auto &draw : commands) {
            if ((skipCaptured && draw.captured) ||
                !casterFrustum.intersects(draw.worldBounds)) continue;
            if (!hasDraws) {
              vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
              vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                      &frame.viewModelScene, 0, nullptr);
              hasDraws = true;
            }
            submitShadowCaster(command, draw, 0, bindings);
            ++statistics.drawCalls.viewModelSunShadow;
          }
        };
        if (layer == 0) {
          // Sampled only where matching baked probe visibility is unavailable.
          casters(frame.viewModelCasters, false);
        } else {
          casters(frame.commands, true);
          casters(frame.viewModelCommands, false);
        }
      });
    }
    gpu::GpuScope targetTime(command, "Forward+ viewmodel target", extent);
    viewModelTarget->record(command, [&] {
      gpu::GpuScope colorTime(command, "Forward+ viewmodel color", extent);
      const bool gi = bool(frame.rayInputs.diffuse);
      const auto drawLayout = gi ? giLayout : layout;
      const auto colorPipeline = gi ? giViewModelPipeline : viewModelPipeline;
      const auto earlyPipeline = gi ? giViewModelEarlyPipeline : viewModelEarlyPipeline;
      VkPipeline activePipeline = VK_NULL_HANDLE;
      DrawBindings bindings;
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 1,
                              &frame.viewModelScene, 0, nullptr);
      if (gi)
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 2, 1, &frame.rayInputs.diffuse, 0, nullptr);
      for (const auto &draw : frame.viewModelCommands) {
        if (!draw.cameraVisible) continue;
        const auto pipeline = earlyPipeline && draw.unconditionalCoverage ? earlyPipeline : colorPipeline;
        fragmentStatistics->begin(command, pipeline == earlyPipeline
              ? FragmentStatistics::viewModelEarly : FragmentStatistics::viewModel);
        if (pipeline != activePipeline) {
          vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          activePipeline = pipeline;
        }
        bindings.bind(command, drawLayout, draw.material, draw.geometry->vertices->handle,
                      draw.geometry->indices->handle);
        const uint32_t push[] = {draw.index, 0};
        vkCmdPushConstants(command, drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), push);
        vkCmdDrawIndexed(command, draw.count, 1, draw.source.firstIndex, draw.source.vertexOffset, 0);
        ++statistics.drawCalls.viewModelColor;
        fragmentStatistics->end(command);
      }
    });
  }
} // namespace remix::forward
