#pragma once
#include "forward/resources.h"
#include "platform/gpu_frames.h"
#include "forward/lighting.h"
#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace remix::forward::bake { struct Volume; }
namespace remix::forward::styled_lighting {
  // Publication metadata, never persisted in the bake cache.
  struct SourceLayout {
    uint32_t baseRecords = 0, localFirstRecord = 0, localWords = 0;
    uint32_t surfaceSize = 0, surfacePages = 0, surfaceLayers = 0;
    uint32_t styleGroups = 0;
  };
  using Weights = std::array<Vec4, 32>;
  struct Resources;
  class FieldCache;
  class Publication {
  public:
    ~Publication();
    VkDescriptorBufferInfo probes() const { return m_data.descriptor(); }
    VkDescriptorImageInfo surfaces() const {
      return {VK_NULL_HANDLE, m_surface.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
  private:
    friend class FieldCache;
    std::shared_ptr<Resources> m_resources;
    std::shared_ptr<const bake::Volume> m_source;
    Buffer m_parameters, m_data, m_status;
    Image m_surface;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkEvent m_completed = VK_NULL_HANDLE;
    uint32_t m_surfaceLayers = 0, m_records = 0, m_pixels = 0, m_cursor = 0;
    uint64_t m_bufferBytes = 0, m_imageBytes = 0;
  };
  // Optional render-thread cache. Prepare pipelines outside frame recording.
  // selectFrame is called only after that slot's submission fence has completed.
  // Unsubmitted/abandoned chunks never advance the publication cursor.
  class Cache {
  public:
    static constexpr uint64_t maximumBytes = 512ull << 20;
    static constexpr uint32_t chunkElements = 128 * 1024;
    Cache(VkPhysicalDevice physical, VkDevice device, uint64_t budget = maximumBytes);
    ~Cache();
    void prepare();
    void selectFrame(uint32_t slot);
    std::shared_ptr<const Publication> record(VkCommandBuffer command,
        std::shared_ptr<const bake::Volume> source, const Weights &weights, double seconds, bool useBase = false);
    uint64_t bufferBytes() const;
    uint64_t imageBytes() const;
    uint64_t recordedChunks() const;
  private:
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    uint64_t m_budget, m_storageRange;
    bool m_supported = false;
    std::shared_ptr<Resources> m_resources;
    std::unique_ptr<FieldCache> m_composed;
  };
}
