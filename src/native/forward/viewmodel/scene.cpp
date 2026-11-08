#include "forward/gpu.h"
#include "forward/probes/receiver_mask.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>

namespace remix::forward {
  void Renderer::Gpu::prepareViewModelDescriptors() {
    auto &frame = frames[slot];
    const auto &data = frame.data;
    if (!frame.viewModelCommands.empty()) {
      auto viewModelData = data;
      const bool localShadows = options.localShadows &&
          std::any_of(shadowSelection.active.begin(), shadowSelection.active.end(), [](bool active) { return active; });
      if (localShadows && !viewModelLocalMaps) {
        viewModelLocalMaps = std::make_unique<shadows::Maps>();
        viewModelLocalMaps->initialize(physical, device, shadowPass, options.localShadowLights,
                                       options.localShadowResolution, false);
      }
      const uint32_t sunCoverage = data.clusterInfo[3] & movingSunCoverageFlag;
      const uint32_t sharedLighting = data.clusterInfo[3] & (inactiveLightStylesFlag | clusterConeMaskFlag | clusterChartConeFlag);
      viewModelData.clusterInfo[3] = (localShadows ? viewModelLocalShadowFlag : 0u) |
          (techniquePlan.reflectionResolve || techniquePlan.inlineReflections ? viewModelReflectionFlag : 0u) |
          sunCoverage | sharedLighting;
      if (sunCoverage != 0u) {
        Vec4 coverage{1, 1, -1, -1};
        const shadows::CoverageProjection projection(frame.viewModelProjection);
        for (const auto &draw : frame.commands)
          if (!draw.captured) projection.include(coverage, draw.worldBounds);
        for (const auto &draw : frame.viewModelCommands)
          projection.include(coverage, draw.worldBounds);
        viewModelData.movingSunCoverage.fill(coverage);
      }
      viewModelData.shadows.fill(frame.viewModelProjection);
      viewModelData.splits.fill(std::numeric_limits<float>::max());
      ensure(frame.viewModelConstants, sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      frame.viewModelConstants->write(&viewModelData, sizeof(viewModelData));
      frame.viewModelScene = allocate(frame, sceneLayout);
      const auto constants = frame.viewModelConstants->descriptor();
      const VkDescriptorImageInfo sun{sampler,
          viewModelSun ? viewModelSun->view() : sunMaps->view(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
      const auto &reflectionProbes = frame.reflectionProbes;
      const auto skyConstants = frame.skyConstants->descriptor();
      // Borrow the completed world hierarchy; viewmodels never enter these guides.
      struct ScreenData {
        Matrix inverseCamera;
        Vec4 trace;
        std::array<uint32_t, 4> config;
        std::array<uint32_t, 4> probeCandidates;
      };
      static_assert(sizeof(ScreenData) == 112);
      Bounds receivers;
      for (const auto &draw : frame.viewModelCommands) {
        if (!draw.worldBounds.valid) { receivers = {}; break; }
        receivers.merge(draw.worldBounds);
      }
      const auto *header = reflectionHeader();
      // Match the normal bias in the sky/probe query, not just the mesh bounds.
      auto candidates = header ? probes::receiverMask(*header, receivers, options.reflectionSampleBias())
                               : std::array<uint32_t, 2>{};
      static const bool unculled = std::getenv("REMIX_DEBUG_FORWARD_VIEWMODEL_UNCULLED_PROBES") != nullptr;
      if (unculled) candidates = {~0u, ~0u};
      const auto screen = reflections ? reflections->screenInputs() : Reflections::ScreenInputs{};
      const ScreenData screenData{screen.depth.imageView ? scene::inverse(data.camera) : scene::identity(),
          {options.ssrDistance / options.sceneScale, options.ssrThickness / options.sceneScale,
           options.reflectionSampleBias(), options.ssrRoughness},
          {uint32_t(samples), options.ssrScale, screen.depth.imageView ? options.ssrSteps : 0u,
           uint32_t(std::bit_width(std::max(extent.width, extent.height)))},
          {candidates[0], candidates[1], 0, 0}};
      ensure(frame.viewModelReflectionConstants, sizeof(ScreenData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      frame.viewModelReflectionConstants->write(&screenData, sizeof(screenData));
      const auto screenConstants = frame.viewModelReflectionConstants->descriptor();
      const VkDescriptorImageInfo guides[]{screen.depth.imageView ? screen.depth : frame.skyImage,
          screen.normals.imageView ? screen.normals : frame.skyImage,
          screen.color.imageView ? screen.color : frame.skyImage};
      const VkDescriptorImageInfo self{sampler, localShadows ? viewModelLocalMaps->dynamicView() : VK_NULL_HANDLE,
          VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
      const auto buffer = [&](uint32_t binding, const VkDescriptorBufferInfo &info,
                              VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
        return VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frame.viewModelScene, .dstBinding = binding, .descriptorCount = 1,
            .descriptorType = type, .pBufferInfo = &info};
      };
      const auto image = [&](uint32_t binding, const VkDescriptorImageInfo &info) {
        return VkWriteDescriptorSet{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frame.viewModelScene, .dstBinding = binding, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &info};
      };
      const VkWriteDescriptorSet writes[]{
          buffer(0, constants), image(4, sun), buffer(12, reflectionProbes.metadata, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
          buffer(13, skyConstants), image(14, frame.skyImage), buffer(15, screenConstants),
          image(16, guides[0]), image(17, guides[1]), image(18, guides[2]), image(19, reflectionProbes.radiance),
          image(11, self)};
      VkCopyDescriptorSet copies[13];
      uint32_t copyCount = 0;
      for (uint32_t binding : {1u, 2u, 3u, 5u, 6u, 7u, 8u, 9u, 10u, 22u, 11u}) {
        // Writes and copies must be disjoint. Without self shadows, binding 11
        // borrows the world descriptor.
        if (binding == 11u && localShadows) continue;
        copies[copyCount++] = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
            .srcSet = frame.scene, .srcBinding = binding, .dstSet = frame.viewModelScene,
            .dstBinding = binding, .descriptorCount = 1};
      }
      vkUpdateDescriptorSets(device, uint32_t(std::size(writes)) - uint32_t(!localShadows), writes, copyCount, copies);
    }
  }
} // namespace remix::forward
