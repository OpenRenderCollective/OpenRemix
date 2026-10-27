#pragma once
#include "forward/probes/image_data.h"
#include "forward/probes/composition.h"
#include "forward/resources.h"
#include <memory>

namespace remix::forward::probes {
  struct CoverageImagePlan;
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
    ImageAsset(VkPhysicalDevice physical, VkDevice device, const Composition &composition);
    ImageAsset(VkPhysicalDevice physical, VkDevice device, const CoverageImagePlan &plan);
    ~ImageAsset();
    ImageAsset(const ImageAsset &) = delete;
    ImageAsset &operator=(const ImageAsset &) = delete;
    void recordUpload(VkCommandBuffer command);
    // Both sources must have an upload recorded on this queue. Their owners
    // must survive the composition submission's fence, just like staging data.
    void recordCompose(VkCommandBuffer command, const ImageAsset &authored, const ImageAsset &automatic);
    // Copy recipe only: caller rechecks scene context and retains both sources
    // through the submission fence. Source metadata is checked before recording.
    void recordSelection(VkCommandBuffer command, const ImageAsset &baseline, const ImageAsset &local);
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
    void recordCopies(VkCommandBuffer command, const ImageAsset &first, const ImageAsset &second);
    void initialize(VkPhysicalDevice physical, const Header &header, const Normalization &normalization = {});
    VkDevice m_device;
    Header m_header;
    Normalization m_normalization;
    VkSampler m_sampler = VK_NULL_HANDLE;
    std::unique_ptr<Buffer> m_metadata, m_staging;
    std::unique_ptr<Image> m_image;
    std::vector<VkBufferImageCopy> m_copies;
    std::vector<LayerCopy> m_layerCopies;
    uint32_t m_layers = 1, m_levels = 1;
    std::unique_ptr<std::array<Header, 2>> m_selectionSources;
    bool m_uploaded = false;
  };
}
