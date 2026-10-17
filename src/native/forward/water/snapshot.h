#pragma once
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace remix::forward::water {
  // An immutable view of the completed opaque frame for water refraction.
  // Outputs are single-sample HDR and nearest opaque depth, independent of
  // the attachments that a later MSAA water pass will load and write.
  class Snapshot {
  public:
    struct Inputs {
      VkImageView hdr = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
      VkDescriptorImageInfo reflections{}; // optional additive specular; preserve its provider's layout
    };
    struct Output {
      VkDescriptorImageInfo hdr, depth;
      VkImage hdrImage, depthImage; // borrowed, for diagnostics/copies; restore their declared layouts
    };
    Snapshot();
    ~Snapshot();
    // Worker preparation; pipeline service must be registered for this device.
    // Inputs later supplied to record() must match this extent/sample count.
    // HDR/reflections are single-sample. Depth is smaller-is-nearer with clear=1.
    // Retire/reinitialize only after all prior uses have completed their fences.
    void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent,
                    VkSampleCountFlagBits samples);
    // Allocate independent images on first use, after the owning slot's fence.
    // Pipeline preparation stays in initialize(); dry scenes need no images.
    void prepareImages();
    // The caller completed this slot's fence. Inputs are completed and in
    // SHADER_READ_ONLY / DEPTH_STENCIL_READ_ONLY layouts. All work remains on
    // the caller's queue; no submission or CPU/GPU wait occurs here.
    // Optional reflections retain the provider's GENERAL or SHADER_READ_ONLY layout.
    void record(VkCommandBuffer command, uint32_t slot, Inputs inputs);
    // Requires prepareImages() or a successful record(); views remain stable until reinitialization.
    Output output() const;
    uint64_t targetBytes() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::forward::water
