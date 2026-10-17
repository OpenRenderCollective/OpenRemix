#pragma once
#include "forward/bindings.h"
#include "forward/options.h"
#include <memory>
#include <vulkan/vulkan.h>

namespace remix::forward::ray {
  class Reflections;
}
namespace remix::forward {
  // Screen-space reflection transport is independent of opaque material shading.
  // Its output is a scene-linear specular contribution with confidence in alpha.
  // Probe and optional RT fallback can share this contract without adding the
  // same specular lobe more than once.
  class Reflections {
  public:
    struct Inputs {
      VkImageView depth, normals, reflectance, hdr;
      VkImage hdrImage;
    };
    struct Environment {
      // Borrowed for this fenced frame: SkyData, published grid, and live dome.
      VkDescriptorBufferInfo sky, irradiance;
      VkDescriptorImageInfo radiance;
    };
    struct ScreenInputs {
      VkDescriptorImageInfo depth, normals, color;
    };
    Reflections();
    ~Reflections();
    void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent,
                    VkSampleCountFlagBits samples, const Options &options, Inputs inputs);
    // Caller has waited for this frame slot's fence. Targets are shared on the
    // renderer's one queue; all cross-pass and cross-frame barriers live here.
    void record(VkCommandBuffer command, uint32_t slot, const FrameData &frame, const Options &options,
                VkDescriptorBufferInfo probes, const Environment &environment,
                ray::Reflections *hardware = nullptr);
    void selectFrame(uint32_t slot); // prior slot fence completed
    VkDescriptorImageInfo output() const;
    // Completed opaque hierarchy and guides, readable by late graphics passes.
    // Borrowed through this fenced frame; empty when screen tracing is disabled.
    ScreenInputs screenInputs() const;
    uint64_t targetBytes() const;
    uint64_t bufferBytes() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::forward
