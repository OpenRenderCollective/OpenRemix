#pragma once
#include "forward/probes/image_data.h"
#include "forward/resources.h"
#include <memory>

namespace remix::forward::probes {
  struct Descriptors {
    VkDescriptorBufferInfo metadata{};
    VkDescriptorImageInfo radiance{};
  };
  // A complete immutable reflection image and its independent projection table.
  // Creation may run on a preparation worker. Upload/publication uses the one
  // render queue; the caller retains this owner through every sampling fence.
  class ImageAsset {
  public:
    ImageAsset(VkPhysicalDevice physical, VkDevice device, const Data &data,
                 std::stop_token cancelled = {});
    ~ImageAsset();
    ImageAsset(const ImageAsset &) = delete;
    ImageAsset &operator=(const ImageAsset &) = delete;
    void recordUpload(VkCommandBuffer command);
    // Only after the upload submission's fence has completed.
    void releaseUpload();
    VkDescriptorBufferInfo metadata() const { return m_metadata->descriptor(); }
    VkDescriptorImageInfo radiance() const;
    Descriptors descriptors() const { return {metadata(), radiance()}; }
    const Image &image() const { return *m_image; }
    const Header &header() const { return m_header; }
    uint64_t bufferBytes() const;
    uint64_t imageBytes() const { return m_image->bytes; }
  private:
    VkDevice m_device;
    Header m_header;
    VkSampler m_sampler = VK_NULL_HANDLE;
    std::unique_ptr<Buffer> m_metadata, m_staging;
    std::unique_ptr<Image> m_image;
    std::vector<VkBufferImageCopy> m_copies;
    uint32_t m_layers = 1, m_levels = 1;
    bool m_uploaded = false;
  };
}
