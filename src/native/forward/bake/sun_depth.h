#pragma once
#include "forward/bake/receiver_projection.h"
#include "forward/bake/transport.h"
#include "forward/resources.h"
#include <atomic>
#include <filesystem>
namespace remix::forward::bake {
  // Immutable capture layout and resources retained by the published volume.
  // Only the render thread records writes, and publication waits for all fences.
  class SunDepth {
  public:
    static std::shared_ptr<SunDepth> prepare(VkPhysicalDevice physical, VkDevice device,
        const scene::BakeScene &source, const lightmap::Layout &receivers, std::stop_token cancelled,
        bool diskCache = false, uint32_t textureResolution = 256);
    float distance() const { return m_header.sampling[2] - m_header.sampling[1]; }
    uint32_t tasks() const { return uint32_t(m_layout.tiles.size()) * m_extent * m_extent; }
    std::vector<Transport::DepthRay> batch(uint32_t first, uint32_t count) const;
    VkDescriptorBufferInfo table() const { return m_table.descriptor(); }
    VkDescriptorBufferInfo values() const { return m_values.descriptor(); }
    bool restored() const { return m_restored; }
    bool cacheEnabled() const { return !m_cacheFile.empty(); }
    uint64_t depthBytes() const { return m_layout.depthBytes; }
    void recordUpload(VkCommandBuffer command);
    void releaseUpload();
    void recordReadback(VkCommandBuffer command, VkPhysicalDevice physical, VkDevice device);
    void storeCache(std::stop_token cancelled); // worker, only after readback submission fence
    uint64_t bytes() const { return m_table.allocationBytes + m_values.allocationBytes + m_transferBytes.load(); }
  private:
    SunDepthLayout m_layout;
    SunDepthHeader m_header;
    std::array<Vec3, 3> m_basis;
    uint32_t m_extent = 0;
    Buffer m_table, m_values;
    std::unique_ptr<Buffer> m_transfer;
    std::atomic<uint64_t> m_transferBytes = 0;
    uint64_t m_cacheKey = 0;
    std::filesystem::path m_cacheFile;
    bool m_restored = false;
    std::vector<float> loadCache(std::stop_token cancelled);
  };
}
