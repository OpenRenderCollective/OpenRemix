#include "forward/bake/sun_depth.h"
#include "forward/bake/sun_depth_cache.h"
#include "forward/bake/cache_file.h"
#include <cstdio>
#include <random>
namespace remix::forward::bake {
  std::vector<float> SunDepth::loadCache(std::stop_token cancelled) {
    if (!cacheEnabled()) return {};
    try {
      const uint64_t expected = sizeof(cache::detail::Header) + 8 + sizeof(SunDepthHeader) + 16 +
          m_layout.directory.size()*sizeof(uint32_t) + uint64_t(tasks())*sizeof(float);
      if (!std::filesystem::exists(m_cacheFile)) return {};
      if (std::filesystem::file_size(m_cacheFile) != expected) throw std::runtime_error("Sun depth cache file size mismatch");
      cache::detail::Reader input(m_cacheFile, m_cacheKey, cancelled);
      auto values = sun_depth_cache::read(input, m_layout, m_header, cancelled);
      input.finish();
      std::fprintf(stderr, "Forward sparse sun depth cache hit: %s\n", m_cacheFile.string().c_str());
      return values;
    } catch (const std::exception &error) {
      if (!cancelled.stop_requested()) std::fprintf(stderr, "Forward sparse sun depth cache miss: %s\n", error.what());
      return {};
    }
  }
  void SunDepth::recordUpload(VkCommandBuffer command) {
    if (!m_restored || !m_transfer) return;
    const VkBufferCopy copy{0,0,m_layout.depthBytes};
    vkCmdCopyBuffer(command, m_transfer->handle, m_values.handle, 1, &copy);
    const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,1,&barrier,0,nullptr,0,nullptr);
  }
  void SunDepth::releaseUpload() { m_transfer.reset(); m_transferBytes = 0; }
  void SunDepth::recordReadback(VkCommandBuffer command, VkPhysicalDevice physical, VkDevice device) {
    if (!cacheEnabled() || m_restored || m_transfer) throw std::logic_error("Invalid sun depth cache readback state");
    m_transfer = std::make_unique<Buffer>();
    m_transfer->create(physical, device, m_layout.depthBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
    m_transferBytes = m_transfer->allocationBytes;
    const VkBufferCopy copy{0,0,m_layout.depthBytes};
    vkCmdCopyBuffer(command, m_values.handle, m_transfer->handle, 1, &copy);
    const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&barrier,0,nullptr,0,nullptr);
  }
  void SunDepth::storeCache(std::stop_token cancelled) {
    std::filesystem::path temporary;
    try {
      cache::detail::check(cancelled);
      if (!m_transfer || m_restored || !cacheEnabled()) throw std::logic_error("Sun depth cache has no completed readback");
      std::vector<float> values(size_t(m_layout.depthBytes/sizeof(float)));
      m_transfer->read(values.data(), values.size()*sizeof(float));
      std::filesystem::create_directories(m_cacheFile.parent_path());
      std::random_device random;
      temporary = m_cacheFile;
      temporary += ".tmp-" + std::to_string((uint64_t(random())<<32)^random());
      if (std::filesystem::exists(temporary)) throw std::runtime_error("Sun depth cache temporary file exists");
      cache::detail::Writer output(temporary, m_cacheKey, cancelled);
      sun_depth_cache::write(output, m_layout, m_header, values, cancelled);
      output.finish();
      cache::detail::check(cancelled);
      cache::detail::replaceFile(temporary, m_cacheFile);
      std::fprintf(stderr, "Forward sparse sun depth cache stored: %s\n", m_cacheFile.string().c_str());
    } catch (const std::exception &error) {
      if (!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary, ignored); }
      if (!cancelled.stop_requested()) std::fprintf(stderr, "Forward sparse sun depth cache write unavailable: %s\n", error.what());
    }
    releaseUpload();
  }
}
