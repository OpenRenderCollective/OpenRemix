#include "forward/gpu.h"
#include "platform/gpu_timers.h"

namespace remix::forward {
  namespace {
    void viewport(VkCommandBuffer command, VkExtent2D extent) {
      const VkViewport view{0, 0, float(extent.width), float(extent.height), 0, 1};
      const VkRect2D scissor{{0, 0}, extent};
      vkCmdSetViewport(command, 0, 1, &view);
      vkCmdSetScissor(command, 0, 1, &scissor);
    }
  } // namespace
  void Renderer::Gpu::drawGeometry(VkCommandBuffer command) {
    const auto &frame = frames[slot];
    const bool gi = bool(frame.giScene);
    const auto drawLayout = gi ? giLayout : layout;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 1, &frame.scene, 0,
                            nullptr);
    if (gi)
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 2, 1, &frame.giScene, 0,
                              nullptr);
    for (const auto &draw : frame.commands) {
      if (!draw.source.rasterVisible)
        continue;
      if (!intersectsClip(draw.worldBounds, frame.data.camera))
        continue;
      const auto *mapped = draw.lightmap;
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        gi       ? (mapped ? giLightmapPipeline : giOpaquePipeline)
                        : mapped ? lightmapPipeline
                                 : opaquePipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 1, 1, &draw.material, 0,
                              nullptr);
      const uint32_t push[] = {draw.index, 0};
      vkCmdPushConstants(command, drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(push), push);
      const VkDeviceSize offset = 0;
      const auto vertices = mapped ? mapped->vertices->handle : draw.geometry->vertices->handle;
      const auto indices = mapped ? mapped->indices->handle : draw.geometry->indices->handle;
      vkCmdBindVertexBuffers(command, 0, 1, &vertices, &offset);
      vkCmdBindIndexBuffer(command, indices, 0, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(command, mapped ? mapped->count : draw.count, 1, mapped ? 0 : draw.source.firstIndex,
                       mapped ? 0 : draw.source.vertexOffset, 0);
    }
  }
  void Renderer::Gpu::drawSunCasters(VkCommandBuffer command, uint32_t cascade, bool staticCasters) {
    const auto &frame = frames[slot];
    const auto &commands = staticCasters ? frame.staticCommands : frame.commands;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frame.scene, 0, nullptr);
    for (const auto &draw : commands) {
      if ((!staticCasters && draw.captured) || !intersectsClip(draw.worldBounds, frame.data.shadows[cascade]))
        continue;
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &draw.material, 0,
                              nullptr);
      const uint32_t push[] = {draw.index, cascade};
      vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(push), push);
      const VkDeviceSize offset = 0;
      vkCmdBindVertexBuffers(command, 0, 1, &draw.geometry->vertices->handle, &offset);
      vkCmdBindIndexBuffer(command, draw.geometry->indices->handle, 0, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(command, draw.count, 1, draw.source.firstIndex, draw.source.vertexOffset, 0);
    }
  }
  void Renderer::Gpu::drawLocalCasters(VkCommandBuffer command, uint32_t lightSlot, uint32_t face,
                                       bool staticCasters) {
    const auto &frame = frames[slot];
    const auto &commands = staticCasters ? frame.staticCommands : frame.commands;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, localShadowPipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frame.scene, 0, nullptr);
    for (const auto &draw : commands) {
      if ((!staticCasters && draw.captured) ||
          !shadows::intersects(draw.worldBounds, shadowSelection.views[lightSlot]))
        continue;
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &draw.material, 0,
                              nullptr);
      const uint32_t push[] = {draw.index, lightSlot * shadows::faces + face};
      vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(push), push);
      const VkDeviceSize offset = 0;
      vkCmdBindVertexBuffers(command, 0, 1, &draw.geometry->vertices->handle, &offset);
      vkCmdBindIndexBuffer(command, draw.geometry->indices->handle, 0, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(command, draw.count, 1, draw.source.firstIndex, draw.source.vertexOffset, 0);
    }
  }
  void Renderer::Gpu::recordWorld(VkCommandBuffer command) {
    auto &frame = frames[slot];
    gpu::GpuScope clusterTime(command, "Forward+ light clustering");
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, clusterPipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &frame.scene, 0, nullptr);
    vkCmdDispatch(command, (frame.data.grid[3] + 63) / 64, 1, 1);
    const VkBufferMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .buffer = frame.clusters->handle,
                                        .offset = 0,
                                        .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    clusterTime.close();
    // Always clear/transition each layer, including when sunlight is disabled:
    // descriptors remain valid and later switches need no special layout state.
    const VkClearValue shadowClear{.depthStencil = {1, 0}};
    const VkExtent2D size{options.shadowResolution, options.shadowResolution};
    gpu::GpuScope sunTime(command, "Forward+ sun shadow cascades", size);
    for (uint32_t c = 0; c < sunShadowLayers; ++c) {
      const VkRenderPassBeginInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                       .renderPass = shadowPass,
                                       .framebuffer = shadowFramebuffers[c],
                                       .renderArea = {{0, 0}, size},
                                       .clearValueCount = 1,
                                       .pClearValues = &shadowClear};
      vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
      viewport(command, size);
      if (frame.data.target[3])
        drawSunCasters(command, c % cascadeCount, c < cascadeCount);
      vkCmdEndRenderPass(command);
    }
    sunTime.close();
    gpu::GpuScope localTime(command, "Forward+ local shadow maps", options.localShadowResolution,
                            options.localShadowResolution);
    localMaps->record(command, shadowSelection, [&](uint32_t lightSlot, uint32_t face, bool staticCasters) {
      drawLocalCasters(command, lightSlot, face, staticCasters);
    });
    localTime.close();
    gpu::GpuScope worldTime(command, "Forward+ opaque world shading", extent);
    const VkClearValue clear[] = {{.color = {{0, 0, 0, 0}}},
                                  {.depthStencil = {1, 0}},
                                  {.color = {{0, 0, 0, 0}}},
                                  {.color = {{0, 0, 0, 0}}}};
    const VkRenderPassBeginInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                     .renderPass = worldPass,
                                     .framebuffer = worldFramebuffer,
                                     .renderArea = {{0, 0}, extent},
                                     .clearValueCount = options.reflections() ? 4u : 2u,
                                     .pClearValues = clear};
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    viewport(command, extent);
    drawGeometry(command);
    vkCmdEndRenderPass(command);
  }
  void Renderer::composite(VkCommandBuffer command) { m_gpu->display(command, false); }
  void Renderer::background(VkCommandBuffer command) {
    if (m_gpu->frames[m_gpu->slot].skyEnabled)
      m_gpu->display(command, true);
  }
  void Renderer::Gpu::display(VkCommandBuffer command, bool background) {
    const auto &g = *this;
    const auto &frame = g.frames[g.slot];
    viewport(command, g.extent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.compositePipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.compositeLayout, 0, 1,
                            &frame.composite, 0, nullptr);
    const struct {
      float exposure;
      uint32_t encode, samples, reflections, background;
    } push{g.options.exposure, uint32_t(g.encodeOutput), uint32_t(g.samples), uint32_t(bool(g.reflections)),
           uint32_t(background)};
    vkCmdPushConstants(command, g.compositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(command, 3, 1, 0, 0);
  }
} // namespace remix::forward
