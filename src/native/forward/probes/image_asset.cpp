#include "forward/probes/image_asset.h"
#include <algorithm>
#include <stdexcept>

namespace remix::forward::probes {
  ImageAsset::ImageAsset(VkPhysicalDevice physical, VkDevice device, const Data &data,
                          std::stop_token cancelled) : m_device(device) {
    auto packed = prepareImage(data, cancelled);
    const uint32_t count = data.header.parameters[0], size = data.header.parameters[1];
    m_layers = std::max(count, 1u);
    m_levels = std::max(data.header.parameters[2], 1u);
    const VkExtent2D extent = count ? VkExtent2D{size, 6*size} : VkExtent2D{1, 1};
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (sizeof(Header) > properties.limits.maxStorageBufferRange ||
        extent.height > properties.limits.maxImageDimension2D ||
        m_layers > properties.limits.maxImageArrayLayers)
      throw std::length_error("Reflection image exceeds device dimensions or metadata range");
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R16G16B16A16_SFLOAT, &format);
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((format.optimalTilingFeatures & required) != required)
      throw std::runtime_error("Device cannot sample uploaded RGBA16F reflection images");
    m_metadata = std::make_unique<Buffer>();
    m_metadata->create(physical, device, sizeof(Header), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    m_metadata->write(&packed.header, sizeof(Header));
    if (!count) {
      packed.pixels.resize(4); // A valid black descriptor before a map bake exists.
      m_copies.push_back({.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .imageExtent = {1, 1, 1}});
    } else {
      for (uint32_t level = 0; level < m_levels; ++level) {
        const auto &mip = packed.levels[level];
        m_copies.push_back({.bufferOffset = mip.byteOffset,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, count},
                            .imageExtent = {mip.faceSize, 6*mip.faceSize, 1}});
      }
    }
    m_staging = std::make_unique<Buffer>();
    m_staging->create(physical, device, packed.pixels.size()*sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    m_staging->write(packed.pixels.data(), packed.pixels.size()*sizeof(uint16_t));
    m_image = std::make_unique<Image>();
    m_image->create(physical, device, extent, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, m_layers, m_levels, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = float(m_levels-1)};
    check(vkCreateSampler(device, &sampler, nullptr, &m_sampler));
  }
  ImageAsset::~ImageAsset() {
    if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
  }
  void ImageAsset::recordUpload(VkCommandBuffer command) {
    if (m_uploaded) throw std::logic_error("Reflection image upload recorded twice");
    VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = m_image->handle,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, m_levels, 0, m_layers}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    vkCmdCopyBufferToImage(command, m_staging->handle, m_image->handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(m_copies.size()), m_copies.data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    m_uploaded = true;
  }
  void ImageAsset::releaseUpload() {
    if (!m_uploaded) throw std::logic_error("Reflection staging released before recording upload");
    m_staging.reset();
    m_copies.clear();
  }
  VkDescriptorImageInfo ImageAsset::radiance() const {
    return {m_sampler, m_image->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
  uint64_t ImageAsset::bufferBytes() const {
    return m_metadata->allocationBytes + (m_staging ? m_staging->allocationBytes : 0);
  }
}
