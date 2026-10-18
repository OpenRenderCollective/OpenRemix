#pragma once
#include "forward/bake/timing.h"
#include "forward/bake/transport.h"
#include "forward/ray_scene.h"
#include "forward/resources.h"
#include "platform/gpu_frames.h"
#include <optional>

namespace remix::forward::bake {
  struct Transport::Gpu {
    bool aggregateDiffuse = false;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::shared_ptr<const scene::BakeScene> source;
    Options options;
    float sunDistance = 0; // scene units; covers the complete frozen geometry bounds
    std::unique_ptr<Timing> timing;
    RayScene rays;
    std::unique_ptr<Buffer> vertices, triangles, materials, lights, lightNodes, componentLightNodes;
    struct Upload {
      std::unique_ptr<Buffer> source;
      VkBuffer target;
      size_t bytes;
    };
    std::vector<Upload> uploads;
    std::unique_ptr<Image> atlas;
    std::vector<Texture> textures;
    uint32_t resolution = 0, domeLayer = 0;
    bool prepared = false, hasDielectrics = false, hasModulatingDecals = false;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE, atlasSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE, atlasLayout = VK_NULL_HANDLE;
    VkPipeline tracePipeline = VK_NULL_HANDLE, parallelPipeline = VK_NULL_HANDLE,
               visibilityPipeline = VK_NULL_HANDLE, atlasPipeline = VK_NULL_HANDLE;
    VkPipeline aggregatePipeline = VK_NULL_HANDLE, aggregateParallelPipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> atlasSets;
    std::vector<VkImageView> atlasViews;
    struct Slot {
      std::unique_ptr<Buffer> constants, points, results;
      std::unique_ptr<Buffer> liveLights;
      std::optional<Frame> lighting;
      struct Fragment {
        std::unique_ptr<Buffer> constants;
        VkDescriptorSet set = VK_NULL_HANDLE;
      };
      std::array<Fragment, size_t(FragmentUse::Count)> fragments;
      VkDescriptorSet set = VK_NULL_HANDLE;
      uint32_t count = 0, capacity = 0;
      uint32_t samples = 0;
      bool componentProposal = false;
      bool compactVisibility = false;
      bool scalarVisibility = false;
    };
    std::array<Slot, gpuFrameCount> slots;
    ~Gpu();
    void createInputs(Inputs inputs);
    void createLayouts();
    void createPipelines();
    void createDescriptors();
    void reserveSlot(uint32_t slot, uint32_t count);
    void recordAtlas(VkCommandBuffer command, RasterRenderer &resources);
    std::unique_ptr<Buffer> buffer(const void *data, size_t bytes, VkBufferUsageFlags usage);
    std::unique_ptr<Buffer> batchBuffer(size_t bytes, bool results);
    std::unique_ptr<Buffer> staticBuffer(const void *data, size_t bytes);
  };
} // namespace remix::forward::bake
