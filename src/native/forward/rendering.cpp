#include "forward/gpu.h"
#include "platform/gpu_timers.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace remix::forward {
  namespace {
    void viewport(VkCommandBuffer command, VkExtent2D extent) {
      const VkViewport view{0, 0, float(extent.width), float(extent.height), 0, 1};
      const VkRect2D scissor{{0, 0}, extent};
      vkCmdSetViewport(command, 0, 1, &view);
      vkCmdSetScissor(command, 0, 1, &scissor);
    }
  } // namespace
  void Renderer::Gpu::drawGeometry(VkCommandBuffer command, bool terrain, bool depthOnly) {
    const auto &frame = frames[slot];
    const bool gi = !depthOnly && bool(frame.giScene);
    static const bool debugDraws = std::getenv("REMIX_DEBUG_FORWARD_DRAW_COUNTS") != nullptr;
    static thread_local uint64_t reported = 0;
    if (debugDraws && !depthOnly && !terrain && baking->volume() && baking->volume()->hash != reported) {
      std::array<uint64_t, 3> counts{}, indices{};
      for (const auto &draw : frame.commands) {
        if (!draw.source.rasterVisible || !intersectsClip(draw.worldBounds, frame.data.camera)) continue;
        const uint32_t kind = !draw.lightmap ? 0 : draw.lightmap->fullyMapped ? 1 : 2;
        ++counts[kind];
        indices[kind] += draw.lightmap ? draw.lightmap->count : draw.count;
      }
      std::fprintf(stderr, "Forward visible draw pipelines: unmapped=%llu/%llu indices, mapped=%llu/%llu indices, "
                           "partial=%llu/%llu indices, liveGI=%u\n",
                   static_cast<unsigned long long>(counts[0]), static_cast<unsigned long long>(indices[0]),
                   static_cast<unsigned long long>(counts[1]), static_cast<unsigned long long>(indices[1]),
                   static_cast<unsigned long long>(counts[2]), static_cast<unsigned long long>(indices[2]), unsigned(gi));
      reported = baking->volume()->hash;
    }
    const auto drawLayout = gi ? giLayout : layout;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 1, &frame.scene, 0,
                            nullptr);
    if (gi)
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 2, 1, &frame.giScene, 0,
                              nullptr);
    auto drawCommand = [&](const Command &draw) {
      if (!draw.source.rasterVisible)
        return;
      if (!intersectsClip(draw.worldBounds, frame.data.camera))
        return;
      const auto *mapped = draw.lightmap;
      VkPipeline selected;
      if (depthOnly) selected = mapped ? depthLightmapPipeline : depthPipeline;
      else if (gi) selected = terrain ? (mapped ? giTerrainLightmapPipeline : giTerrainPipeline)
                                     : (mapped ? giLightmapPipeline : giOpaquePipeline);
      else if (terrain) selected = !mapped ? terrainPipeline : mapped->fullyMapped ? terrainLightmapPipeline
                                                                                  : terrainPartialLightmapPipeline;
      else selected = !mapped ? opaquePipeline : mapped->fullyMapped ? lightmapPipeline : partialLightmapPipeline;
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, selected);
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
    };
    // Time consecutive receiver runs without changing coplanar/decal ordering.
    // Terrain blending and depth passes keep their ordinary timing scopes.
    static const bool profileReceivers = std::getenv("REMIX_DEBUG_FORWARD_RECEIVER_TIMINGS") != nullptr;
    const bool receiverTimings = profileReceivers && gpu::timing();
    if ((receiverTimings || fragmentStatistics->enabled()) && !terrain && !depthOnly && !gi) {
      const char *names[] = {"Forward+ probe-lit receivers", "Forward+ lightmapped receivers",
                             "Forward+ partially mapped receivers"};
      std::optional<gpu::GpuScope> receiverTime;
      uint32_t kind = 3;
      for (const auto &draw : frame.commands) {
        if (!draw.source.rasterVisible || !intersectsClip(draw.worldBounds, frame.data.camera)) continue;
        const uint32_t receiver = !draw.lightmap ? 0 : draw.lightmap->fullyMapped ? 1 : 2;
        if (receiver != kind) {
          receiverTime.reset();
          fragmentStatistics->end(command);
          fragmentStatistics->begin(command, receiver);
          if (receiverTimings) receiverTime.emplace(command, names[receiver], extent);
          kind = receiver;
        }
        drawCommand(draw);
      }
      fragmentStatistics->end(command);
    } else {
      fragmentStatistics->begin(command, depthOnly ? 3 : terrain ? 4 : 5);
      for (const auto &draw : terrain ? frame.terrainCommands : frame.commands) drawCommand(draw);
      fragmentStatistics->end(command);
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
  bool Renderer::Gpu::castsLocalShadow(const Command &draw, uint32_t lightSlot, bool staticCasters) const {
    return (staticCasters || !draw.captured) &&
           shadows::intersects(draw.worldBounds, shadowSelection.views[lightSlot]);
  }
  void Renderer::Gpu::drawLocalCasters(VkCommandBuffer command, uint32_t lightSlot, uint32_t face,
                                       bool staticCasters) {
    const auto &frame = frames[slot];
    const auto &commands = staticCasters ? frame.staticCommands : frame.commands;
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, localShadowPipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frame.scene, 0, nullptr);
    for (const auto &draw : commands) {
      // Radial overlap alone submits every caster to all six faces. Keep
      // seam-crossing bounds on each touched face, including offscreen actors.
      if (!castsLocalShadow(draw, lightSlot, staticCasters) ||
          !intersectsClip(draw.worldBounds, shadowSelection.views[lightSlot].matrices[face]))
        continue;
      auto &counts = statistics.localShadowDraws[lightSlot];
      ++(staticCasters ? counts.frozen : counts.moving)[face];
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
    statistics.localShadowDraws = {};
    for (uint32_t i = 0; i < shadows::maximumLights; ++i) {
      statistics.localShadowDraws[i].active = shadowSelection.active[i];
      statistics.localShadowDraws[i].lightId = shadowSelection.views[i].id;
    }
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
      if (c < cascadeCount && !frame.staticSunDirty[c])
        continue;
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
    fragmentStatistics->reset(command, extent, samples, baking->volume() ? baking->volume()->hash : 0);
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
    if (options.depthPrepass) {
      gpu::GpuScope depthTime(command, "Forward+ opaque depth prepass", extent);
      drawGeometry(command, false, true);
    }
    drawGeometry(command);
    vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
    drawGeometry(command, true);
    vkCmdEndRenderPass(command);
  }
  void Renderer::composite(VkCommandBuffer command) { m_gpu->display(command, false); }
  void Renderer::Gpu::recordWater(VkCommandBuffer command) {
    auto &frame = frames[slot];
    frame.waterComposed = false;
    if (!waterPass || !std::any_of(frame.waterCommands.begin(), frame.waterCommands.end(), [&](const auto &draw) {
          return draw.source.rasterVisible && intersectsClip(draw.worldBounds, frame.data.camera);
        })) return;
    gpu::GpuScope time(command, "Forward+ measured water", extent);
    const auto specular = reflections ? reflections->output() : VkDescriptorImageInfo{};
    waterPass->record(command, slot,
        {resolved ? resolved->view : color->view, depth->view, specular},
        [&](VkPipeline pipeline, VkPipelineLayout waterLayout) {
          vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, waterLayout, 0, 1,
                                  &frame.scene, 0, nullptr);
          for (const auto &draw : frame.waterCommands) {
            if (!draw.source.rasterVisible || !intersectsClip(draw.worldBounds, frame.data.camera)) continue;
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, waterLayout, 1, 1,
                                    &draw.material, 0, nullptr);
            const uint32_t push[] = {draw.index, draw.waterIndex};
            vkCmdPushConstants(command, waterLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), push);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command, 0, 1, &draw.geometry->vertices->handle, &offset);
            vkCmdBindIndexBuffer(command, draw.geometry->indices->handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command, draw.count, 1, draw.source.firstIndex, draw.source.vertexOffset, 0);
          }
        }, frame.waterRayScene);
    frame.waterComposed = true;
  }
  void Renderer::background(VkCommandBuffer command) {
    if (m_gpu->frames[m_gpu->slot].skyEnabled || m_gpu->frames[m_gpu->slot].waterMedium)
      m_gpu->display(command, true);
  }
  void Renderer::Gpu::display(VkCommandBuffer command, bool background) {
    const auto &g = *this;
    const auto &frame = g.frames[g.slot];
    viewport(command, g.extent);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.compositePipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.compositeLayout, 0, 1,
                            &frame.composite, 0, nullptr);
    const auto post = g.postProcessing->bindings();
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.compositeLayout, 1, 1,
                            &post, 0, nullptr);
    const struct {
      uint32_t prism, encode, samples, reflections, background, gamma22, hardwareSrgb, viewModel;
    } push{uint32_t(g.options.prism), uint32_t(g.encodeOutput), uint32_t(g.samples),
           uint32_t(bool(g.reflections) && !frame.waterComposed && g.options.debugView == 0),
           uint32_t(background), uint32_t(g.options.gamma22), uint32_t(g.hardwareSrgb),
           uint32_t(!frame.viewModelCommands.empty())};
    vkCmdPushConstants(command, g.compositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(command, 3, 1, 0, 0);
  }
} // namespace remix::forward
