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
      viewModelData.clusterInfo[3] = uint32_t(localShadows) | (uint32_t(options.reflections()) << 1);
      viewModelData.shadows.fill(frame.viewModelProjection);
      viewModelData.splits.fill(std::numeric_limits<float>::max());
      ensure(frame.viewModelConstants, sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      frame.viewModelConstants->write(&viewModelData, sizeof(viewModelData));
      frame.viewModelScene = allocate(frame, sceneLayout);
      VkCopyDescriptorSet copies[12];
      for (uint32_t binding = 0; binding < 12; ++binding)
        copies[binding] = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
            .srcSet = frame.scene, .srcBinding = binding, .dstSet = frame.viewModelScene,
            .dstBinding = binding, .descriptorCount = 1};
      vkUpdateDescriptorSets(device, 0, nullptr, 12, copies);
      const VkCopyDescriptorSet probeCopy{.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
          .srcSet = frame.scene, .srcBinding = 22, .dstSet = frame.viewModelScene,
          .dstBinding = 22, .descriptorCount = 1};
      vkUpdateDescriptorSets(device, 0, nullptr, 1, &probeCopy);
      const auto constants = frame.viewModelConstants->descriptor();
      const VkDescriptorImageInfo sun{shadowSampler,
          viewModelSun ? viewModelSun->view() : shadow->view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
      const VkWriteDescriptorSet overrides[] = {
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.viewModelScene,
           .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
           .pBufferInfo = &constants},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.viewModelScene,
           .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           .pImageInfo = &sun}};
      vkUpdateDescriptorSets(device, 2, overrides, 0, nullptr);
      const auto reflectionProbes = reflectionDescriptor();
      const auto skyConstants = frame.skyConstants->descriptor();
      const VkWriteDescriptorSet environment[] = {
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.viewModelScene, .dstBinding = 12,
           .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &reflectionProbes.metadata},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.viewModelScene, .dstBinding = 13,
           .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &skyConstants},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.viewModelScene, .dstBinding = 14,
           .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &frame.skyImage},
          {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.viewModelScene, .dstBinding = 19,
           .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
           .pImageInfo = &reflectionProbes.radiance}};
      vkUpdateDescriptorSets(device, 4, environment, 0, nullptr);
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
      auto candidates = header ? probes::receiverMask(*header, receivers, .002f / options.sceneScale)
                               : std::array<uint32_t, 2>{};
      static const bool unculled = std::getenv("REMIX_DEBUG_FORWARD_VIEWMODEL_UNCULLED_PROBES") != nullptr;
      if (unculled) candidates = {~0u, ~0u};
      const auto screen = reflections ? reflections->screenInputs() : Reflections::ScreenInputs{};
      const ScreenData screenData{scene::inverse(data.camera),
          {options.ssrDistance / options.sceneScale, options.ssrThickness / options.sceneScale,
           .002f / options.sceneScale, options.ssrRoughness},
          {uint32_t(samples), options.ssrScale, options.ssr && screen.depth.imageView ? options.ssrSteps : 0u,
           uint32_t(std::bit_width(std::max(extent.width, extent.height)))},
          {candidates[0], candidates[1], 0, 0}};
      ensure(frame.viewModelReflectionConstants, sizeof(ScreenData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      frame.viewModelReflectionConstants->write(&screenData, sizeof(screenData));
      const auto screenConstants = frame.viewModelReflectionConstants->descriptor();
      const VkWriteDescriptorSet screenBuffer{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frame.viewModelScene, .dstBinding = 15, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &screenConstants};
      vkUpdateDescriptorSets(device, 1, &screenBuffer, 0, nullptr);
      const VkDescriptorImageInfo guides[]{screen.depth.imageView ? screen.depth : frame.skyImage,
          screen.normals.imageView ? screen.normals : frame.skyImage,
          screen.color.imageView ? screen.color : frame.skyImage};
      VkWriteDescriptorSet screenImages[3];
      for (uint32_t i = 0; i < 3; ++i)
        screenImages[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frame.viewModelScene, .dstBinding = 16 + i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &guides[i]};
      vkUpdateDescriptorSets(device, 3, screenImages, 0, nullptr);
      if (localShadows) {
        const VkDescriptorImageInfo self{shadowSampler, viewModelLocalMaps->dynamicView(),
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = frame.viewModelScene, .dstBinding = 11, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &self};
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
      }
    }
  }
} // namespace remix::forward
