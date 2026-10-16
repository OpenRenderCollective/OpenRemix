#include "forward/bake/gpu.h"
#include "raster/renderer.h"

namespace remix::forward::bake {
  void Transport::Gpu::recordAtlas(VkCommandBuffer command, RasterRenderer &resources) {
    const VkImageMemoryBarrier initialize{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = atlas->handle,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, uint32_t(textures.size() + 1)}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &initialize);
    const VkClearColorValue white{{1, 1, 1, 1}};
    vkCmdClearColorImage(command, atlas->handle, VK_IMAGE_LAYOUT_GENERAL, &white, 1,
                         &initialize.subresourceRange);
    for (size_t i = 0; i < textures.size(); ++i) {
      const VkDescriptorImageInfo output{VK_NULL_HANDLE, atlasViews[i], VK_IMAGE_LAYOUT_GENERAL};
      const auto input = resources.sampledTexture(textures[i].sampler, textures[i].image, command);
      const VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                              .dstSet = atlasSets[i],
                                              .dstBinding = 0,
                                              .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              .pImageInfo = &input},
                                             {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                              .dstSet = atlasSets[i],
                                              .dstBinding = 1,
                                              .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                              .pImageInfo = &output}};
      vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }
    // The shared provider transitions uploads for its raster consumers. Include
    // those writes/layout transitions before this compute consumer as well.
    const VkMemoryBarrier upload{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         1, &upload, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, atlasPipeline);
    for (uint32_t i = 0; i < atlasSets.size(); ++i) {
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, atlasLayout, 0, 1, &atlasSets[i], 0,
                              nullptr);
      vkCmdDispatch(command, (resolution + 7) / 8, (resolution + 7) / 8, 1);
    }
    const VkMemoryBarrier ready{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1,
                         &ready, 0, nullptr, 0, nullptr);
  }
} // namespace remix::forward::bake
