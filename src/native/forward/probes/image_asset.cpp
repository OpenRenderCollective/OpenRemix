#include "forward/probes/image_asset.h"
#include "forward/probes/coverage_image_plan.h"
#include <cstring>
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace remix::forward::probes {
  ImageAsset::ImageAsset(VkPhysicalDevice physical, VkDevice device, const Data &data,
                          std::stop_token cancelled) : m_device(device) {
    auto packed = prepareImage(data, cancelled);
    const uint32_t count = packed.header.parameters[0];
    m_levels = std::max(packed.header.parameters[2], 1u);
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
    initialize(physical, packed.header);
  }
  ImageAsset::ImageAsset(VkPhysicalDevice physical, VkDevice device, const Composition &composition)
      : m_device(device), m_layerCopies(composition.layers) {
    if (m_layerCopies.empty() || m_layerCopies.size() != composition.header.parameters[0])
      throw std::invalid_argument("Invalid reflection composition layer count");
    initialize(physical, composition.header);
  }
  ImageAsset::ImageAsset(VkPhysicalDevice physical, VkDevice device, const CoverageImagePlan &plan)
      : m_device(device), m_selectionSources(std::make_unique<std::array<Header, 2>>(plan.sources)) {
    if (plan.copies.empty() || plan.copies.size() != plan.header.parameters[0])
      throw std::invalid_argument("Invalid reflection selection layer count");
    for (const auto &copy : plan.copies) {
      if (copy.source != CoverageImageSource::Baseline && copy.source != CoverageImageSource::Local)
        throw std::invalid_argument("Unknown reflection selection source");
      m_layerCopies.push_back({copy.source == CoverageImageSource::Local, copy.layer});
    }
    initialize(physical, plan.header);
  }
  void ImageAsset::initialize(VkPhysicalDevice physical, const Header &header) {
    m_header = header;
    const uint32_t count = header.parameters[0], size = header.parameters[1];
    if (count > maxProbes || (count && (size < 8 || size > 512 || !std::has_single_bit(size) ||
        header.parameters[2] != std::bit_width(size) ||
        header.parameters[3] != levelOffset(size, header.parameters[2]))))
      throw std::invalid_argument("Invalid reflection image metadata");
    m_layers = std::max(count, 1u);
    m_levels = std::max(header.parameters[2], 1u);
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
    m_metadata->create(physical, m_device, sizeof(Header),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    m_metadata->write(&m_header, sizeof(Header));
    m_image = std::make_unique<Image>();
    m_image->create(physical, m_device, extent, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, m_layers, m_levels, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = float(m_levels-1)};
    check(vkCreateSampler(m_device, &sampler, nullptr, &m_sampler));
  }
  ImageAsset::~ImageAsset() {
    if (m_sampler) vkDestroySampler(m_device, m_sampler, nullptr);
  }
  void ImageAsset::recordUpload(VkCommandBuffer command) {
    if (m_uploaded || !m_staging) throw std::logic_error("Reflection image has no pending staging upload");
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
  void ImageAsset::recordCompose(VkCommandBuffer command, const ImageAsset &authored, const ImageAsset &automatic) {
    if (m_selectionSources) throw std::logic_error("Reflection selection requires recordSelection");
    recordCopies(command, authored, automatic);
  }
  void ImageAsset::recordSelection(VkCommandBuffer command, const ImageAsset &baseline, const ImageAsset &local) {
    if (!m_selectionSources) throw std::logic_error("Reflection image has no selection recipe");
    if (std::memcmp(&(*m_selectionSources)[0], &baseline.m_header, sizeof(Header)) ||
        std::memcmp(&(*m_selectionSources)[1], &local.m_header, sizeof(Header)))
      throw std::invalid_argument("Reflection selection source metadata changed");
    recordCopies(command, baseline, local);
  }
  void ImageAsset::recordCopies(VkCommandBuffer command, const ImageAsset &first, const ImageAsset &second) {
    if (m_uploaded || m_layerCopies.empty() || !first.m_uploaded || !second.m_uploaded ||
        this == &first || this == &second || m_device != first.m_device || m_device != second.m_device)
      throw std::logic_error("Reflection composition requires uploaded independent sources");
    std::array<std::vector<VkImageCopy>, 2> copies;
    const ImageAsset *sources[]{&first, &second};
    for (uint32_t layer = 0; layer < m_layerCopies.size(); ++layer) {
      const auto source = m_layerCopies[layer];
      const auto &asset = *sources[source.automatic];
      if (source.layer >= asset.m_header.parameters[0] ||
          asset.m_header.parameters[1] != m_header.parameters[1] || asset.m_levels != m_levels)
        throw std::invalid_argument("Reflection composition source dimensions or layer mismatch");
      for (uint32_t level = 0; level < m_levels; ++level) {
        const auto size = std::max(m_header.parameters[1] >> level, 1u);
        copies[source.automatic].push_back({
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, source.layer, 1},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}, .extent = {size, 6*size, 1}});
      }
    }
    std::vector<VkImageMemoryBarrier> barriers;
    auto barrier = [](const ImageAsset &asset, VkImageLayout oldLayout, VkImageLayout newLayout,
                      VkAccessFlags source, VkAccessFlags destination) {
      return VkImageMemoryBarrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .srcAccessMask = source, .dstAccessMask = destination, .oldLayout = oldLayout, .newLayout = newLayout,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = asset.m_image->handle,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, asset.m_levels, 0, asset.m_layers}};
    };
    for (uint32_t source = 0; source < 2; ++source)
      if (!copies[source].empty() && !(source == 1 && sources[0] == sources[1] && !copies[0].empty()))
        barriers.push_back(barrier(*sources[source], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT));
    barriers.push_back(barrier(*this, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               0, VK_ACCESS_TRANSFER_WRITE_BIT));
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
    for (uint32_t source = 0; source < 2; ++source)
      if (!copies[source].empty())
        vkCmdCopyImage(command, sources[source]->m_image->handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       m_image->handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       uint32_t(copies[source].size()), copies[source].data());
    for (auto &item : barriers) {
      item.srcAccessMask = item.dstAccessMask;
      item.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      item.oldLayout = item.newLayout;
      item.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
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
