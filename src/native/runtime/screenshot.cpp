#include "runtime/screenshot.h"
#include "platform/utf8_path.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <image/stb_image_write.h>

namespace remix {
  namespace {
    std::filesystem::path nativePath(const char *path) { return utf8Path(path); }
    void check(VkResult result) {
      if (result != VK_SUCCESS)
        throw result;
    }
    // An IEEE half's bits to float.
    float halfFloat(uint16_t h) {
      const uint32_t sign = uint32_t(h & 0x8000u) << 16, exponent = (h >> 10) & 31u, mantissa = h & 0x3ffu;
      uint32_t bits;
      if (exponent == 31)
        bits = sign | 0x7f800000u | mantissa << 13;
      else if (exponent)
        bits = sign | (exponent + 112) << 23 | mantissa << 13;
      else if (mantissa) {
        float value = float(mantissa) / 16777216.f; // 2^-24
        return sign ? -value : value;
      } else
        bits = sign;
      float value;
      std::memcpy(&value, &bits, sizeof(value));
      return value;
    }
  } // namespace

  remixapi_ErrorCode PresentedScreenshot::request(const char *path, bool supported) {
    const std::filesystem::path destination = nativePath(path);
    if (!destination.is_absolute())
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (destination.extension() != ".png" || !supported)
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    if (!m_requested.empty() || !m_captured.empty())
      return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    m_requested = path;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  void PresentedScreenshot::record(VkPhysicalDevice physical, VkDevice device, VkCommandBuffer command,
                                   VkImage image, VkExtent2D extent, VkFormat format, VkImageLayout layout) {
    if (m_requested.empty() || !m_captured.empty())
      return;
    m_half = format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkDeviceSize size = VkDeviceSize(extent.width) * extent.height * (m_half ? 8 : 4);
    if (!size || size > 512u * 1024u * 1024u) {
      SDL_Log("Screenshot dimensions exceed the readback limit");
      m_requested.clear();
      return;
    }
    m_extent = extent;
    m_bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = size,
                                     .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    check(vkCreateBuffer(device, &info, nullptr, &m_buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, m_buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    uint32_t type = properties.memoryTypeCount;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      if ((requirements.memoryTypeBits & (1u << i)) &&
          (properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        type = i;
        break;
      }
    }
    if (type == properties.memoryTypeCount)
      throw VK_ERROR_FEATURE_NOT_PRESENT;
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                             .allocationSize = requirements.size,
                                             .memoryTypeIndex = type};
    check(vkAllocateMemory(device, &allocation, nullptr, &m_memory));
    check(vkBindBufferMemory(device, m_buffer, m_memory, 0));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                    .oldLayout = layout,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {extent.width, extent.height, 1}};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_buffer, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = layout;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    const VkBufferMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .buffer = m_buffer,
                                        .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                         1, &host, 0, nullptr);
    m_captured = std::exchange(m_requested, {});
  }

  void PresentedScreenshot::submitted(VkFence fence) {
    if (!m_captured.empty() && !m_fence)
      m_fence = fence;
  }

  void PresentedScreenshot::complete(VkDevice device, bool wait) {
    if (m_captured.empty() || !m_fence)
      return;
    const auto ready = wait ? vkWaitForFences(device, 1, &m_fence, VK_TRUE, 1000000000)
                            : vkGetFenceStatus(device, m_fence);
    if (ready != VK_SUCCESS)
      return;
    try {
      // Allocate before mapping, so allocation failure cannot leave memory mapped.
      std::vector<unsigned char> pixels(size_t(m_extent.width) * m_extent.height * 4);
      std::vector<uint16_t> halves(m_half ? pixels.size() : 0);
      void *mapped = nullptr;
      check(vkMapMemory(device, m_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
      const VkMappedMemoryRange range = {
          .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = m_memory, .size = VK_WHOLE_SIZE};
      const auto result = vkInvalidateMappedMemoryRanges(device, 1, &range);
      if (result == VK_SUCCESS) {
        if (m_half)
          std::memcpy(halves.data(), mapped, halves.size() * sizeof(uint16_t));
        else
          std::memcpy(pixels.data(), mapped, pixels.size());
      }
      vkUnmapMemory(device, m_memory);
      check(result);
      for (size_t i = 0; i < halves.size(); ++i) {
        const float value = halfFloat(halves[i]);
        pixels[i] = (unsigned char)(std::lround(std::clamp(value == value ? value : 0.f, 0.f, 1.f) * 255));
      }
      for (size_t i = 0; i < pixels.size(); i += 4) {
        if (m_bgra)
          std::swap(pixels[i], pixels[i + 2]);
        pixels[i + 3] = 255;
      }
      std::ofstream output(nativePath(m_captured.c_str()), std::ios::binary);
      const auto writer = [](void *context, void *data, int size) {
        static_cast<std::ofstream *>(context)->write(static_cast<char *>(data), size);
      };
      const int written =
          output && stbi_write_png_to_func(writer, &output, static_cast<int>(m_extent.width),
                                           static_cast<int>(m_extent.height), 4, pixels.data(),
                                           static_cast<int>(m_extent.width * 4));
      output.flush();
      if (!written || !output)
        SDL_Log("Screenshot write failed: %s", m_captured.c_str());
    } catch (...) {
      SDL_Log("Screenshot readback failed: %s", m_captured.c_str());
    }
    reset(device);
  }

  void PresentedScreenshot::reset(VkDevice device, bool keepRequest) {
    if (m_buffer)
      vkDestroyBuffer(device, m_buffer, nullptr);
    if (m_memory)
      vkFreeMemory(device, m_memory, nullptr);
    m_buffer = VK_NULL_HANDLE;
    m_memory = VK_NULL_HANDLE;
    m_fence = VK_NULL_HANDLE;
    if (!keepRequest)
      m_requested.clear();
    m_captured.clear();
  }
} // namespace remix
