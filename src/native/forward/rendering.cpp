#include "forward/gpu.h"
#include "forward/material.h"
#include "forward/lightmap/upload.h"
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
  void Renderer::Gpu::DrawBindings::bind(VkCommandBuffer command, VkPipelineLayout layout,
      VkDescriptorSet material, VkBuffer vertices, VkBuffer indices) {
    if (material != m_material) {
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &material, 0, nullptr);
      m_material = material;
    }
    if (vertices != m_vertices) {
      const VkDeviceSize offset = 0;
      vkCmdBindVertexBuffers(command, 0, 1, &vertices, &offset);
      m_vertices = vertices;
    }
    if (indices != m_indices) {
      vkCmdBindIndexBuffer(command, indices, 0, VK_INDEX_TYPE_UINT32);
      m_indices = indices;
    }
  }

  void Renderer::Gpu::drawGeometry(VkCommandBuffer command, bool terrain, bool depthOnly, bool indirect) {
    const auto &frame = frames[slot];
    const bool gi = !depthOnly && bool(frame.rayInputs.diffuse);
    // Publication can fall back to legacy fields on any bake. Only select
    // the indexed variant from the flags actually supplied to this draw pass.
    const bool indexed = indexedLightmapPipeline && frame.data.clusterInfo[2] != 0u &&
        (frame.data.occlusion[0] & lightmap::indexedVisibilityFlag) != 0u;
    const auto mappedPipeline = indexed ? indexedLightmapPipeline : lightmapPipeline;
    if (depthOnly && frame.depthBatch && frame.depthBatch->indexCount()) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, depthBatchPipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frame.scene, 0, nullptr);
      const VkBuffer buffers[] = {frame.depthBatch->positions().handle, frame.depthBatch->tags().handle};
      const VkDeviceSize offsets[] = {0, 0};
      vkCmdBindVertexBuffers(command, 0, 2, buffers, offsets);
      vkCmdBindIndexBuffer(command, frame.depthBatch->indices().handle, 0, VK_INDEX_TYPE_UINT32);
      fragmentStatistics->begin(command, 3);
      vkCmdDrawIndexed(command, frame.depthBatch->indexCount(), 1, 0, 0, 0);
      fragmentStatistics->end(command);
      ++statistics.drawCalls.worldDepth;
      ++statistics.drawCalls.capturedDepth;
      statistics.drawCalls.worldDepthLogical += frame.depthBatch->visibleSources();
      statistics.drawCalls.batchedDepthSources = frame.depthBatch->visibleSources();
    }
    static const bool debugDraws = std::getenv("REMIX_DEBUG_FORWARD_DRAW_COUNTS") != nullptr;
    static thread_local uint64_t reported = 0;
    if (debugDraws && !depthOnly && !terrain && baking->volume() && baking->volume()->hash != reported) {
      std::array<uint64_t, 3> counts{}, indices{}, receiverRuns{};
      for (const auto &draw : frame.commands) {
        if (!draw.cameraVisible) continue;
        const uint32_t kind = !draw.lightmap ? 0 : draw.lightmap->fullyMapped ? 1 : 2;
        ++counts[kind];
        indices[kind] += draw.lightmap ? draw.lightmap->count : draw.count;
        if (!gi && draw.lightmap && !draw.lightmap->receiverRuns.empty()) {
          for (const auto &run : draw.lightmap->receiverRuns)
            ++receiverRuns[run.coverage == lightmap::ReceiverCoverage::Unmapped ? 0
                         : run.coverage == lightmap::ReceiverCoverage::Mapped ? 1 : 2];
        } else ++receiverRuns[kind];
      }
      std::fprintf(stderr, "Forward visible draw pipelines: unmapped=%llu/%llu indices, mapped=%llu/%llu indices, "
                           "partial=%llu/%llu indices, liveGI=%u\n",
                   static_cast<unsigned long long>(counts[0]), static_cast<unsigned long long>(indices[0]),
                   static_cast<unsigned long long>(counts[1]), static_cast<unsigned long long>(indices[1]),
                   static_cast<unsigned long long>(counts[2]), static_cast<unsigned long long>(indices[2]), unsigned(gi));
      std::fprintf(stderr, "Forward emitted receiver runs: unmapped=%llu mapped=%llu mixed=%llu\n",
                   static_cast<unsigned long long>(receiverRuns[0]),
                   static_cast<unsigned long long>(receiverRuns[1]),
                   static_cast<unsigned long long>(receiverRuns[2]));
      if (indexed)
        std::fprintf(stderr, "Forward indexed lightmap world pipeline active\n");
      reported = baking->volume()->hash;
    }
    const auto drawLayout = gi ? giLayout : layout;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 1, &frame.scene, 0,
                            nullptr);
    if (gi)
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 2, 1, &frame.rayInputs.diffuse, 0,
                              nullptr);
    // State persists between consecutive draws. Keep this cache local to the
    // pass so other cameras/passes cannot leave stale bindings behind.
    VkPipeline boundPipeline = VK_NULL_HANDLE;
    DrawBindings bindings;
    static const bool profileReceivers = std::getenv("REMIX_DEBUG_FORWARD_RECEIVER_TIMINGS") != nullptr;
    const bool receiverTimings = profileReceivers && gpu::timing();
    const bool receiverStatistics = (receiverTimings || fragmentStatistics->enabled()) &&
                                    !terrain && !depthOnly && !gi;
    std::optional<gpu::GpuScope> receiverTime;
    uint32_t receiverKind = 3;
    auto selectReceiver = [&](VkPipeline pipeline) {
      if (!receiverStatistics) return;
      const uint32_t kind = pipeline == mappedPipeline || pipeline == indexedRoughDielectricPipeline
          ? 1 : pipeline == partialLightmapPipeline ? 2 : 0;
      if (kind == receiverKind) return;
      receiverTime.reset();
      fragmentStatistics->end(command);
      fragmentStatistics->begin(command, kind);
      const char *names[] = {"Forward+ probe-lit receivers", "Forward+ lightmapped receivers",
                             "Forward+ partially mapped receivers"};
      if (receiverTimings) receiverTime.emplace(command, names[kind], extent);
      receiverKind = kind;
    };
    VkDeviceSize indirectOffset = 0;
    auto drawIndexed = [&](const VkDrawIndexedIndirectCommand &range) {
      if (indirect) {
        vkCmdDrawIndexedIndirect(command, frame.indirectDraws->handle, indirectOffset, 1,
                                 sizeof(VkDrawIndexedIndirectCommand));
        indirectOffset += sizeof(VkDrawIndexedIndirectCommand);
      } else {
        vkCmdDrawIndexed(command, range.indexCount, range.instanceCount, range.firstIndex,
                         range.vertexOffset, range.firstInstance);
      }
    };
    auto drawCommand = [&](const Command &draw) {
      if (depthOnly && draw.depthBatched) return;
      if (!draw.cameraVisible) return;
      const auto *mapped = draw.lightmap;
      VkPipeline selected;
      if (depthOnly) selected = mapped ? depthLightmapPipeline : depthPipeline;
      else if (gi) selected = terrain ? (mapped ? giTerrainLightmapPipeline : giTerrainPipeline)
                                     : (mapped ? giLightmapPipeline : giOpaquePipeline);
      else if (terrain) selected = !mapped ? terrainPipeline : mapped->fullyMapped ? terrainLightmapPipeline : terrainPartialLightmapPipeline;
      else selected = !mapped ? opaquePipeline : mapped->fullyMapped ? mappedPipeline : partialLightmapPipeline;
      constexpr uint32_t roughnessOrMetallicTexture = 1u | 2u;
      const bool roughDielectric = !depthOnly && !terrain && !gi && mapped && indexed &&
          indexedRoughDielectricPipeline && !(drawScratch[draw.index].maps[0] & roughnessOrMetallicTexture) &&
          drawScratch[draw.index].pbr[0] == 1.f && drawScratch[draw.index].pbr[1] == 0.f;
      const VkPipeline drawMapped = roughDielectric ? indexedRoughDielectricPipeline : mappedPipeline;
      if (selected == mappedPipeline) selected = drawMapped;
      auto bindPipeline = [&](VkPipeline pipeline) {
        if (pipeline == indexedRoughDielectricPipeline) {
          static thread_local bool reportedRoughDielectric = false;
          if (!reportedRoughDielectric && debugDraws) {
            std::fprintf(stderr, "Forward indexed rough dielectric pipeline active\n");
            reportedRoughDielectric = true;
          }
        }
        selectReceiver(pipeline);
        if (pipeline != boundPipeline) {
          vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          boundPipeline = pipeline;
        }
      };
      const uint32_t push[] = {draw.index, 0, draw.reflectionCandidates[0], draw.reflectionCandidates[1]};
      const uint32_t pushBytes = techniquePlan.inlineReflections && !depthOnly && !terrain ? 16u : 8u;
      vkCmdPushConstants(command, drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         pushBytes, push);
      const auto vertices = mapped ? mapped->vertices->handle : draw.geometry->vertices->handle;
      const auto indices = mapped ? mapped->indices->handle : draw.geometry->indices->handle;
      bindings.bind(command, drawLayout, draw.material, vertices, indices);
      auto emit = [&](const auto &range, auto coverage) {
        const VkPipeline pipeline = !coverage ? selected
            : *coverage == lightmap::ReceiverCoverage::Mapped ? drawMapped
            : *coverage == lightmap::ReceiverCoverage::Unmapped ? unmappedLightmapPipeline : partialLightmapPipeline;
        auto &calls = statistics.drawCalls;
        bindPipeline(pipeline);
        drawIndexed(range);
        ++(depthOnly ? calls.worldDepth : terrain ? calls.terrain : calls.worldColor);
        if (depthOnly && draw.captured) {
          ++calls.capturedDepth;
          calls.unconditionalCapturedDepth += uint32_t(draw.unconditionalCoverage);
        }
        if (depthOnly) ++calls.worldDepthLogical;
      };
      draw.forEachIndexRange(!depthOnly && !terrain && !gi, emit);
    };
    // Attribute actual emitted receiver runs, rather than their source mesh.
    // Partial source meshes can use both mapped and probe-only pipelines.
    // Group consecutive runs without changing coplanar/decal draw order.
    if (!receiverStatistics)
      fragmentStatistics->begin(command, depthOnly ? 3 : terrain ? 4 : 5);
    for (const auto &draw : terrain ? frame.terrainCommands : frame.commands) drawCommand(draw);
    receiverTime.reset();
    fragmentStatistics->end(command);
  }

  void Renderer::Gpu::submitShadowCaster(VkCommandBuffer command, const Command &draw, uint32_t layer,
                                        DrawBindings &bindings) {
    bindings.bind(command, layout, draw.material, draw.geometry->vertices->handle, draw.geometry->indices->handle);
    const uint32_t push[] = {draw.index, layer};
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), push);
    vkCmdDrawIndexed(command, draw.count, 1, draw.source.firstIndex, draw.source.vertexOffset, 0);
  }

  void Renderer::Gpu::drawSunCasters(VkCommandBuffer command, uint32_t cascade, bool staticCasters) {
    const auto &frame = frames[slot];
    const auto &commands = staticCasters ? frame.staticCommands : frame.commands;
    const ClipFrustum frustum(frame.data.shadows[cascade]);
    uint32_t draws = 0;
    DrawBindings bindings;
    for (const auto &draw : commands) {
      if ((!staticCasters && draw.captured) || !frustum.intersects(draw.worldBounds))
        continue;
      if (draws == 0) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frame.scene, 0, nullptr);
      }
      submitShadowCaster(command, draw, cascade, bindings);
      ++draws;
    }
    if (staticCasters) statistics.drawCalls.staticSunShadow += draws;
    else {
      statistics.drawCalls.movingSunShadow += draws;
      statistics.movingSunDraws[cascade] = draws;
    }
  }
  void Renderer::Gpu::drawLocalCasters(VkCommandBuffer command, uint32_t lightSlot, uint32_t face,
                                       bool staticCasters) {
    const auto &frame = frames[slot];
    const auto &view = shadowSelection.views[lightSlot];
    const auto &candidates = staticCasters ? frame.staticLocalCasters[lightSlot] : frame.movingLocalCasters[lightSlot];
    if (candidates.empty()) return;
    const ClipFrustum frustum(view.matrices[face]);
    const auto &commands = staticCasters ? frame.staticCommands : frame.commands;
    auto &counts = statistics.localShadowDraws[lightSlot];
    auto &faceDraws = (staticCasters ? counts.frozen : counts.moving)[face];
    DrawBindings bindings;
    for (uint32_t index : candidates) {
      const auto &draw = commands[index];
      // Keep seam-crossing bounds on each touched face, including offscreen actors.
      if (!frustum.intersects(draw.worldBounds)) continue;
      if (faceDraws++ == 0) {
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, localShadowPipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frame.scene, 0, nullptr);
      }
      submitShadowCaster(command, draw, lightSlot * shadows::faces + face, bindings);
      ++(staticCasters ? statistics.drawCalls.staticLocalShadow : statistics.drawCalls.movingLocalShadow);
    }
  }
  void Renderer::Gpu::recordWorld(VkCommandBuffer command) {
    auto &frame = frames[slot];
    statistics.drawCalls = {};
    statistics.movingSunDraws = {};
    statistics.localShadowDraws = {};
    for (uint32_t i = 0; i < shadows::maximumLights; ++i) {
      statistics.localShadowDraws[i].active = shadowSelection.active[i];
      statistics.localShadowDraws[i].lightId = shadowSelection.views[i].id;
    }
    // Empty receiver lists and unclustered fallback do not read cluster headers.
    if (statistics.clusterBuildLights != 0) {
      gpu::GpuScope clusterTime(command, "Forward+ light clustering");
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, clusterPipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &frame.scene, 0, nullptr);
      const uint32_t charts = (frame.data.clusterInfo[3] & clusterChartConeFlag) ? frame.data.occlusion[1] : 0;
      vkCmdDispatch(command, (std::max(frame.data.grid[3], charts) + 63) / 64, 1, 1);
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
    }
    // Initialization and enable/source transitions clear every layer. Once
    // disabled, those neutral maps remain valid without repeating empty passes.
    const VkClearValue shadowClear{.depthStencil = {1, 0}};
    const VkExtent2D size{options.shadowResolution, options.shadowResolution};
    gpu::GpuScope sunTime(command, "Forward+ sun shadow cascades", size);
    statistics.movingSunUpdates = 0;
    for (uint32_t c = 0; c < sunShadowLayers; ++c) {
      if (!frame.staticSunDirty[c % cascadeCount] && (c < cascadeCount || !frame.data.target[3]))
        continue;
      const VkRenderPassBeginInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                       .renderPass = shadowPass,
                                       .framebuffer = sunMaps->framebuffer(c),
                                       .renderArea = {{0, 0}, size},
                                       .clearValueCount = 1,
                                       .pClearValues = &shadowClear};
      vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
      viewport(command, size);
      if (frame.data.target[3])
        drawSunCasters(command, c % cascadeCount, c < cascadeCount);
      vkCmdEndRenderPass(command);
      statistics.movingSunUpdates += uint32_t(c >= cascadeCount);
    }
    sunTime.close();
    gpu::GpuScope localTime(command, "Forward+ local shadow maps", options.localShadowResolution,
                            options.localShadowResolution);
    std::array<bool, shadows::maximumLights> movingDirty{};
    statistics.movingLocalUpdates = statistics.movingLocalReuses = 0;
    for (uint32_t lightSlot = 0; lightSlot < shadows::maximumLights; ++lightSlot) {
      auto &cache = movingShadowCache[lightSlot];
      if (!shadowSelection.active[lightSlot]) { cache.reset(); continue; }
      movingCasterScratch.clear();
      for (uint32_t index : frame.movingLocalCasters[lightSlot]) {
        const auto &draw = frame.commands[index];
        movingCasterScratch.push_back({draw.source.surface, draw.source.bones, draw.source.model,
            draw.source.firstIndex, draw.count, draw.source.vertexOffset, draw.opaqueDepth,
            draw.opaqueDepth ? shadows::AlphaCoverage{} :
                shadows::alphaCoverage(drawScratch[draw.index], *draw.source.material, draw.alphaSampler)});
        if ((!draw.opaqueDepth && !movingCasterScratch.back().alpha.cacheable) ||
            movingCasterScratch.size() > shadows::MovingCache::maximumCasters) break;
      }
      movingDirty[lightSlot] = cache.update(shadowSelection.views[lightSlot], movingCasterScratch);
      ++(movingDirty[lightSlot] ? statistics.movingLocalUpdates : statistics.movingLocalReuses);
    }
    movingCasterScratch.clear();
    localMaps->record(command, shadowSelection, [&](uint32_t lightSlot, uint32_t face, bool staticCasters) {
      drawLocalCasters(command, lightSlot, face, staticCasters);
    }, movingDirty);
    localTime.close();
    const uint32_t visibilityBoundCount = prepareOpaqueVisibility();
    fragmentStatistics->reset(command, extent, samples, baking->volume() ? baking->volume()->hash : 0);
    gpu::GpuScope worldTime(command, "Forward+ opaque world shading", extent);
    const VkClearValue clear[] = {{.color = {{0, 0, 0, 0}}},
                                  {.depthStencil = {1, 0}},
                                  {.color = {{0, 0, -1, -1}}},
                                  {.color = {{0, 0, 0, 0}}},
                                  {.color = {{0, 0, 0, 0}}}};
    const VkRenderPassBeginInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                     .renderPass = worldPass,
                                     .framebuffer = worldFramebuffer,
                                     .renderArea = {{0, 0}, extent},
                                     .clearValueCount = (techniquePlan.reflectionResolve ? 4u : 2u) + uint32_t(options.autoExposure),
                                     .pClearValues = clear};
    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    viewport(command, extent);
    if (options.depthPrepass) {
      gpu::GpuScope depthTime(command, "Forward+ opaque depth prepass", extent);
      drawGeometry(command, false, true);
    }
    const bool useVisibility = visibilityBoundCount != 0;
    static const bool reportVisibility = std::getenv("REMIX_DEBUG_FORWARD_DRAW_COUNTS") != nullptr;
    if (opaqueResumePass && reportVisibility) {
      static thread_local std::optional<bool> previous;
      if (previous != useVisibility) {
        std::fprintf(stderr, "Forward opaque GPU visibility: %s\n", useVisibility ? "active" : "bypassed");
        previous = useVisibility;
      }
    }
    if (useVisibility) {
      vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdEndRenderPass(command);
      recordOpaqueVisibility(command, visibilityBoundCount);
      auto resume = pass;
      resume.renderPass = opaqueResumePass;
      vkCmdBeginRenderPass(command, &resume, VK_SUBPASS_CONTENTS_INLINE);
      viewport(command, extent);
    }
    drawGeometry(command, false, false, useVisibility);
    vkCmdNextSubpass(command, VK_SUBPASS_CONTENTS_INLINE);
    drawGeometry(command, true);
    vkCmdEndRenderPass(command);
  }
  void Renderer::composite(VkCommandBuffer command) { m_gpu->display(command, false); }
  void Renderer::Gpu::recordWater(VkCommandBuffer command) {
    auto &frame = frames[slot];
    frame.waterComposed = false;
    if (!waterPass || !std::any_of(frame.waterCommands.begin(), frame.waterCommands.end(), [](const auto &draw) {
          return draw.cameraVisible;
        })) return;
    gpu::GpuScope time(command, "Forward+ measured water", extent);
    // This pass also overlays opaque reflections, even when water is occluded.
    // Keep diagnostic exclusions consistent with the final compositor.
    const auto specular = reflections && options.debugView == 0 ? reflections->output() : VkDescriptorImageInfo{};
    waterPass->record(command, slot,
        {resolved ? resolved->view : color->view, depth->view, specular},
        [&](VkPipeline pipeline, VkPipelineLayout waterLayout) {
          vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, waterLayout, 0, 1,
                                  &frame.scene, 0, nullptr);
          for (const auto &draw : frame.waterCommands) {
            if (!draw.cameraVisible) continue;
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, waterLayout, 1, 1,
                                    &draw.material, 0, nullptr);
            const uint32_t push[] = {draw.index, draw.waterIndex};
            vkCmdPushConstants(command, waterLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), push);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(command, 0, 1, &draw.geometry->vertices->handle, &offset);
            vkCmdBindIndexBuffer(command, draw.geometry->indices->handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command, draw.count, 1, draw.source.firstIndex, draw.source.vertexOffset, 0);
            ++statistics.drawCalls.water;
          }
        }, {frame.rayInputs.water, frame.rayInputs.diffuse,
            frame.rayInputs.reflectionDistance, frame.rayInputs.reflectionBounces});
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
      uint32_t hdr10;
      float peakNits, paperWhiteNits;
    } push{uint32_t(g.options.prism), uint32_t(g.encodeOutput), uint32_t(g.samples),
           uint32_t(bool(g.reflections) && !frame.waterComposed && g.options.debugView == 0),
           uint32_t(background), uint32_t(g.options.gamma22), uint32_t(g.hardwareSrgb),
           uint32_t(!frame.viewModelCommands.empty()), uint32_t(g.displayOutput.hdr10),
           g.displayOutput.peakNits, g.displayOutput.paperWhiteNits};
    vkCmdPushConstants(command, g.compositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(command, 3, 1, 0, 0);
  }
} // namespace remix::forward
