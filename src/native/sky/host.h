#pragma once
// The GPU objects sky::Renderer takes and hands out. NUMOS and renderer.cpp
// are shared by both backends: Vulkan builds record through sky/bridge.h,
// Metal builds through metal/sky_bridge.h (the same Bridge over the Metal
// device). On Vulkan these are the Vulkan types themselves.
#if !defined(REMIX_METAL)
#include <vulkan/vulkan.h>

namespace remix::sky {
  using PhysicalDevice = VkPhysicalDevice;
  using Device = VkDevice;
  using CommandBuffer = VkCommandBuffer;
  using Image = VkImage;
  using ImageView = VkImageView;
  using AccelerationStructure = VkAccelerationStructureKHR;
  using SampledImage = VkDescriptorImageInfo;
  // The sky's images stay in VK_IMAGE_LAYOUT_GENERAL.
  inline VkDescriptorImageInfo sampledImage(VkSampler sampler, VkImageView view) {
    return {sampler, view, VK_IMAGE_LAYOUT_GENERAL};
  }
} // namespace remix::sky
#else
#import <Metal/Metal.h>
#include <cstddef>
#include <vector>

namespace remix::sky {
  using PhysicalDevice = std::nullptr_t;
  using Device = id<MTLDevice>;
  using CommandBuffer = id<MTLCommandBuffer>;
  // A Metal texture is its own image and view.
  using Image = id<MTLTexture>;
  using ImageView = id<MTLTexture>;
  // The scene's TLAS and the acceleration structures its instances reference
  // (made resident for the passes that trace it). The tracer owns it and
  // refreshes it each frame; the sky reads it as it records.
  struct MetalScene {
    id<MTLAccelerationStructure> tlas = nil;
    std::vector<id<MTLResource>> referenced;
  };
  using AccelerationStructure = const MetalScene *;
  struct SampledImage {
    id<MTLSamplerState> sampler = nil;
    id<MTLTexture> texture = nil;
  };
  inline SampledImage sampledImage(id<MTLSamplerState> sampler, id<MTLTexture> texture) { return {sampler, texture}; }
} // namespace remix::sky
#endif
