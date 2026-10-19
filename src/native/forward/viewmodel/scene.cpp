#include "forward/gpu.h"
#include <limits>

namespace remix::forward {
  void Renderer::Gpu::prepareViewModelDescriptors() {
    auto &frame = frames[slot];
    const auto &data = frame.data;
    if (!frame.viewModelCommands.empty()) {
      auto viewModelData = data;
      viewModelData.shadows.fill(frame.viewModelProjection);
      viewModelData.splits.fill(std::numeric_limits<float>::max());
      ensure(frame.viewModelConstants, sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      frame.viewModelConstants->write(&viewModelData, sizeof(viewModelData));
      frame.viewModelScene = allocate(frame, sceneLayout);
      VkCopyDescriptorSet copies[11];
      for (uint32_t binding = 0; binding < 11; ++binding)
        copies[binding] = {.sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
            .srcSet = frame.scene, .srcBinding = binding, .dstSet = frame.viewModelScene,
            .dstBinding = binding, .descriptorCount = 1};
      vkUpdateDescriptorSets(device, 0, nullptr, 11, copies);
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
    }
  }
} // namespace remix::forward
