#pragma once
#include "forward/water/data.h"
#include "forward/water/snapshot.h"
#include "forward/probes/image_asset.h"
#include <functional>
#include <span>

namespace remix::forward::water {
  struct Parameters {
    Matrix projection, inverseCamera;
    Vec4 cameraMedium{}, target{}, settings{}, domeUp{}, sunFog{}, reflectionTrace{};
    std::array<uint32_t, 4> reflectionConfig{};
  };
  static_assert(sizeof(Parameters) == 240);
  // Owns the late body/sheet refraction pass and independent sampled snapshots. The
  // renderer keeps its existing MSAA colour/depth attachments and draw owners.
  class Pass {
  public:
    struct Techniques {
      bool rayReflections = false;
      bool liveDiffuse = false;
      bool bakedDirect = false;
    };
    struct Targets {
      VkImageView color, depth, resolved;
    };
    struct Lighting {
      probes::Descriptors probes;
      VkDescriptorBufferInfo sky;
      VkDescriptorImageInfo dome;
      VkDescriptorImageInfo reflectionDepth{}, reflectionNormals{}, reflectionColor{};
    };
    Pass();
    ~Pass();
    static bool supportsRayReflections(VkPhysicalDevice physical);
    void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent,
                    VkSampleCountFlagBits samples, VkDescriptorSetLayout scene,
                    VkDescriptorSetLayout material, Targets targets, Techniques techniques);
    // The selected slot's fence has completed. All provider inputs and draw
    // owners must remain alive through the next completion of that fence.
    void prepare(uint32_t slot, const Parameters &parameters, std::span<const MaterialData> materials,
                 Lighting lighting);
    VkDescriptorBufferInfo parameters(uint32_t slot) const;
    // All commands remain on the caller's ordered queue. draw receives the
    // water pipeline/layout with set 2 already bound; it binds scene/material
    // sets and submits the retained water geometry.
    struct RayInputs {
      VkDescriptorSet reflections = VK_NULL_HANDLE, diffuse = VK_NULL_HANDLE;
      float reflectionDistance = 0;
      uint32_t reflectionBounces = 0;
    };
    void record(VkCommandBuffer command, uint32_t slot, Snapshot::Inputs inputs,
                const std::function<void(VkPipeline, VkPipelineLayout)> &draw,
                RayInputs rays);
    uint64_t targetBytes() const;
    uint64_t bufferBytes() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::forward::water
