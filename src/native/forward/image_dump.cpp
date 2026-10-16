#include "forward/image_dump.h"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace remix::forward {
  bool ImageDump::enabled(VkPhysicalDevice physical) {
    const char *prefix = std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX");
    if (!prefix || !*prefix)
      return false;
    for (auto format : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT}) {
      VkFormatProperties properties;
      vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
      if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
        return false;
    }
    return true;
  }
  ImageDump::ImageDump(VkPhysicalDevice physical, VkDevice device) : m_physical(physical), m_device(device) {
    if (const auto *prefix = std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX"))
      m_prefix = prefix;
    if (const auto *full = std::getenv("REMIX_DEBUG_FULL_REFLECTION_INPUTS"))
      m_fullImages = std::string_view(full) == "1";
  }

  bool ImageDump::active(uint32_t slot) {
    if (m_prefix.empty() || m_done || (m_slot && *m_slot != slot))
      return false;
    if (!m_slot) {
      if (!std::filesystem::exists(m_prefix + ".frame.request"))
        return false;
      m_slot = slot;
    }
    return true;
  }
  void ImageDump::record(VkCommandBuffer command, uint32_t slot, const char *name, VkImage image,
                         VkFormat format, VkExtent2D extent, VkImageLayout layout, uint32_t mip) noexcept {
    try {
      if (!active(slot))
        return;
      if (!image || !extent.width || !extent.height ||
          (format != VK_FORMAT_R16G16B16A16_SFLOAT && format != VK_FORMAT_R32_SFLOAT))
        throw std::runtime_error("Unsupported image dump input");
      const uint32_t width = m_fullImages ? extent.width : std::min(512u, extent.width);
      const uint32_t height = m_fullImages ? extent.height : std::min(512u, extent.height);
      const uint32_t x = (extent.width - width) / 2, y = (extent.height - height) / 2;
      const uint32_t pixelBytes = format == VK_FORMAT_R32_SFLOAT ? 4 : 8;
      auto buffer = std::make_unique<Buffer>();
      buffer->create(m_physical, m_device, uint64_t(width) * height * pixelBytes,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      // Establish ownership before recording anything that references it. A
      // failed allocation cannot leave a command referring to a destroyed buffer.
      m_pending.push_back(
          {name, {uint32_t(format), extent.width, extent.height, x, y, width, height}, std::move(buffer)});
      const auto &entry = m_pending.back();
      VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                   .oldLayout = layout,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
      const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1},
                                   .imageOffset = {int32_t(x), int32_t(y), 0},
                                   .imageExtent = {width, height, 1}};
      vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_GENERAL, entry.buffer->handle, 1, &copy);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = layout;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
      const VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host,
                           0, nullptr, 0, nullptr);
    } catch (VkResult error) {
      std::fprintf(stderr, "Image dump allocation unavailable: Vulkan %d\n", int(error));
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Image dump unavailable: %s\n", error.what());
    }
  }
  void ImageDump::recordBuffer(VkCommandBuffer command, uint32_t slot, const char *name,
                               VkDescriptorBufferInfo source) noexcept {
    try {
      if (!active(slot))
        return;
      if (!source.buffer || !source.range || source.range == VK_WHOLE_SIZE)
        throw std::runtime_error("Invalid diagnostic buffer range");
      auto buffer = std::make_unique<Buffer>();
      buffer->create(m_physical, m_device, source.range, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      m_buffers.push_back({name, std::move(buffer), size_t(source.range)});
      const VkMemoryBarrier readable{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                     .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                           &readable, 0, nullptr, 0, nullptr);
      const VkBufferCopy copy{source.offset, 0, source.range};
      vkCmdCopyBuffer(command, source.buffer, m_buffers.back().buffer->handle, 1, &copy);
      const VkMemoryBarrier complete{
          .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
          .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &complete,
                           0, nullptr, 0, nullptr);
    } catch (VkResult error) {
      std::fprintf(stderr, "Buffer dump allocation unavailable: Vulkan %d\n", int(error));
    } catch (const std::exception &error) {
      std::fprintf(stderr, "Buffer dump unavailable: %s\n", error.what());
    }
  }
  void ImageDump::selectFrame(uint32_t slot) noexcept {
    if (!m_slot || *m_slot != slot || m_done)
      return;
    m_done = true;
    for (const auto &entry : m_pending) {
      try {
        static_assert(std::endian::native == std::endian::little);
        const auto path = std::filesystem::path(m_prefix + "." + entry.name + ".image.bin");
        if (std::filesystem::exists(path))
          throw std::runtime_error("Image dump already exists; choose a fresh prefix");
        const uint32_t pixelBytes = entry.metadata[0] == VK_FORMAT_R32_SFLOAT ? 4 : 8;
        std::vector<std::byte> data(size_t(entry.metadata[5]) * entry.metadata[6] * pixelBytes);
        entry.buffer->read(data.data(), data.size());
        std::ofstream output(path, std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.write("RMXIMG01", 8);
        output.write(reinterpret_cast<const char *>(entry.metadata), sizeof(entry.metadata));
        output.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
        output.close();
      } catch (const std::exception &error) {
        std::fprintf(stderr, "Image dump write unavailable: %s\n", error.what());
      } catch (VkResult error) {
        std::fprintf(stderr, "Image dump read unavailable: Vulkan %d\n", int(error));
      }
    }
    m_pending.clear();
    for (const auto &entry : m_buffers) {
      try {
        const auto path = std::filesystem::path(m_prefix + "." + entry.name + ".buffer.bin");
        if (std::filesystem::exists(path))
          throw std::runtime_error("Buffer dump already exists; choose a fresh prefix");
        std::vector<std::byte> data(entry.bytes);
        entry.buffer->read(data.data(), data.size());
        std::ofstream output(path, std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
        output.close();
      } catch (VkResult error) {
        std::fprintf(stderr, "Buffer dump read unavailable: Vulkan %d\n", int(error));
      } catch (const std::exception &error) {
        std::fprintf(stderr, "Buffer dump write unavailable: %s\n", error.what());
      }
    }
    m_buffers.clear();
  }
} // namespace remix::forward
