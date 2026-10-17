#pragma once
#include "forward/bake/controller.h"
#include "forward/bindings.h"
#include "forward/reflections.h"
#include "forward/renderer.h"
#include "forward/resources.h"
#include "forward/shadows/maps.h"
#include "forward/shadows/sun_cache.h"
#include "platform/gpu_frames.h"
#include <chrono>
#include <unordered_map>
#include <vector>

namespace remix::forward {
  struct Renderer::Gpu {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    Options options;
    Statistics statistics;
    bool encodeOutput = false;
    uint32_t slot = 0;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    VkDescriptorSetLayout sceneLayout = VK_NULL_HANDLE, materialLayout = VK_NULL_HANDLE,
                          compositeSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE, compositeLayout = VK_NULL_HANDLE;
    VkPipeline opaquePipeline = VK_NULL_HANDLE, lightmapPipeline = VK_NULL_HANDLE,
               shadowPipeline = VK_NULL_HANDLE, localShadowPipeline = VK_NULL_HANDLE,
               clusterPipeline = VK_NULL_HANDLE, compositePipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE, shadowSampler = VK_NULL_HANDLE, surfaceSampler = VK_NULL_HANDLE,
              skySampler = VK_NULL_HANDLE;
    VkRenderPass worldPass = VK_NULL_HANDLE, shadowPass = VK_NULL_HANDLE;
    VkFramebuffer worldFramebuffer = VK_NULL_HANDLE;
    std::array<VkFramebuffer, sunShadowLayers> shadowFramebuffers{};
    std::array<VkImageView, sunShadowLayers> shadowViews{};
    std::unique_ptr<Image> color, depth, resolved, shadow;
    std::unique_ptr<Image> normalGuide, reflectanceGuide;
    std::unique_ptr<Reflections> reflections;
    std::unique_ptr<ray::Reflections> rayReflections;
    std::unique_ptr<ray::LiveScene> liveScene;
    bool giSupported = false;
    VkDescriptorSetLayout giSceneLayout = VK_NULL_HANDLE;
    VkPipelineLayout giLayout = VK_NULL_HANDLE;
    VkPipeline giOpaquePipeline = VK_NULL_HANDLE, giLightmapPipeline = VK_NULL_HANDLE;
    std::shared_ptr<bake::Controller> baking;
    std::unique_ptr<shadows::Maps> localMaps;
    shadows::Planner shadowPlanner;
    shadows::Selection shadowSelection;
    shadows::SunCache sunCache;
    uint64_t baseTargetBytes = 0;

    struct Geometry {
      std::weak_ptr<scene::Surface> source;
      std::unique_ptr<Buffer> vertices, indices;
      Bounds bounds;
    };
    struct Command {
      std::shared_ptr<Geometry> geometry;
      const lightmap::Geometry::Entry *lightmap = nullptr;
      Bounds worldBounds;
      bool captured = false;
      scene::Draw source; // material, mesh and texture owners live through the fence
      VkDescriptorSet material = VK_NULL_HANDLE;
      uint32_t index = 0, count = 0;
    };
    struct Frame {
      VkDescriptorPool pool = VK_NULL_HANDLE;
      uint32_t poolCapacity = 0;
      VkDescriptorSet scene = VK_NULL_HANDLE, composite = VK_NULL_HANDLE;
      VkDescriptorSet giScene = VK_NULL_HANDLE;
      std::unique_ptr<Buffer> constants, lights, draws, clusters, skyConstants;
      bool skyEnabled = false;
      VkDescriptorImageInfo skyImage{};
      std::vector<Command> commands, staticCommands;
      std::array<bool, cascadeCount> staticSunDirty{};
      FrameData data;
    };
    std::array<Frame, gpuFrameCount> frames;
    std::unordered_map<const scene::Surface *, std::shared_ptr<Geometry>> geometries;

    ~Gpu();
    void createLayouts();
    void createTargets();
    void createPipelines(VkRenderPass output);
    void prepareFrame(VkCommandBuffer command, RasterRenderer &resources);
    void recordWorld(VkCommandBuffer command);
    void drawLocalCasters(VkCommandBuffer command, uint32_t slot, uint32_t face, bool staticCasters);
    void drawGeometry(VkCommandBuffer command);
    void drawSunCasters(VkCommandBuffer command, uint32_t cascade, bool staticCasters);
    void prepareSky(VkCommandBuffer command, RasterRenderer &resources);
    void display(VkCommandBuffer command, bool background);
    std::shared_ptr<Geometry> geometry(const scene::Draw &draw);
    VkDescriptorSet allocate(Frame &frame, VkDescriptorSetLayout layout);
    void ensure(std::unique_ptr<Buffer> &buffer, VkDeviceSize bytes, VkBufferUsageFlags usage,
                Buffer::Memory memory = Buffer::Memory::Host);
  };
} // namespace remix::forward
