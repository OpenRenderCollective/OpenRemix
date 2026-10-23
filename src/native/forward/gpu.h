#pragma once
#include "forward/bake/controller.h"
#include "forward/bindings.h"
#include "forward/fragment_statistics.h"
#include "forward/highlight_selection.h"
#include "forward/reflections.h"
#include "forward/probes/authoring.h"
#include "forward/probe_batch/batch.h"
#include "forward/renderer.h"
#include "forward/resources.h"
#include "forward/shadows/maps.h"
#include "forward/shadows/sun_cache.h"
#include "forward/water/pass.h"
#include "forward/viewmodel/target.h"
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
    std::unique_ptr<FragmentStatistics> fragmentStatistics;
    Statistics statistics;
    bool encodeOutput = false;
    bool hardwareSrgb = false;
    uint32_t slot = 0;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    VkDescriptorSetLayout sceneLayout = VK_NULL_HANDLE, materialLayout = VK_NULL_HANDLE,
                          compositeSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE, compositeLayout = VK_NULL_HANDLE;
    VkPipeline opaquePipeline = VK_NULL_HANDLE, lightmapPipeline = VK_NULL_HANDLE,
               partialLightmapPipeline = VK_NULL_HANDLE,
               depthPipeline = VK_NULL_HANDLE, depthLightmapPipeline = VK_NULL_HANDLE,
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
    std::unique_ptr<water::Pass> waterPass;
    std::unique_ptr<viewmodel::Target> viewModelTarget;
    std::unique_ptr<viewmodel::SunShadow> viewModelSun;
    std::unique_ptr<shadows::Maps> viewModelLocalMaps;
    VkPipeline viewModelPipeline = VK_NULL_HANDLE;
    std::unique_ptr<postfx::Renderer> postProcessing;
    VkDescriptorSetLayout postLayout = VK_NULL_HANDLE;
    std::unique_ptr<ray::Reflections> rayReflections;
    std::unique_ptr<ray::LiveScene> liveScene;
    bool giSupported = false;
    bool waterRaySupported = false;
    VkDescriptorSetLayout giSceneLayout = VK_NULL_HANDLE;
    VkPipelineLayout giLayout = VK_NULL_HANDLE;
    VkPipeline giOpaquePipeline = VK_NULL_HANDLE, giLightmapPipeline = VK_NULL_HANDLE;
    VkPipeline terrainPipeline = VK_NULL_HANDLE, terrainLightmapPipeline = VK_NULL_HANDLE,
               terrainPartialLightmapPipeline = VK_NULL_HANDLE,
               giTerrainPipeline = VK_NULL_HANDLE, giTerrainLightmapPipeline = VK_NULL_HANDLE;
    std::shared_ptr<bake::Controller> baking;
    std::unique_ptr<probe_batch::Cache> probeVisibility;
    std::unique_ptr<probes::Authoring> authoredReflections;
    probes::Descriptors reflectionDescriptor() const {
      return authoredReflections->descriptor(baking->reflectionDescriptor());
    }
    std::unique_ptr<shadows::Maps> localMaps;
    shadows::Planner shadowPlanner;
    shadows::Selection shadowSelection;
    HighlightSelection highlightSelection;
    std::weak_ptr<const mixed::Bindings> highlightSource;
    shadows::SunCache sunCache;
    std::weak_ptr<const scene::BakeScene> staticBoundsSource;
    Bounds staticBounds;
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
      uint32_t waterIndex = 0;
    };
    struct Frame {
      VkDescriptorPool pool = VK_NULL_HANDLE;
      uint32_t poolCapacity = 0;
      VkDescriptorSet scene = VK_NULL_HANDLE, composite = VK_NULL_HANDLE;
      VkDescriptorSet viewModelScene = VK_NULL_HANDLE;
      VkDescriptorSet giScene = VK_NULL_HANDLE;
      VkDescriptorSet waterRayScene = VK_NULL_HANDLE;
      std::unique_ptr<Buffer> constants, lights, draws, clusters, skyConstants;
      std::shared_ptr<const probe_batch::Field> probeVisibility;
      std::unique_ptr<Buffer> viewModelConstants, viewModelReflectionConstants;
      Matrix viewModelProjection = scene::identity();
      bool skyEnabled = false;
      VkDescriptorImageInfo skyImage{};
      std::vector<Command> commands, staticCommands;
      std::vector<Command> terrainCommands;
      std::vector<Command> waterCommands;
      std::vector<Command> viewModelCommands;
      std::vector<Command> viewModelCasters;
      // Rewritten only after this slot's fence. Poses remain fresh every draw;
      // the buffers, rather than the animated vertex data, survive slot reuse.
      std::vector<std::shared_ptr<Geometry>> posedGeometry;
      uint32_t posedUsed = 0;
      bool waterComposed = false;
      bool waterMedium = false;
      std::array<bool, cascadeCount> staticSunDirty{};
      FrameData data;
    };
    std::array<Frame, gpuFrameCount> frames;
    // CPU-only upload scratch. prepareFrame runs serially and Buffer::write
    // copies these records before returning; no submitted frame retains them.
    // Reuse capacity rather than reallocating/copying the draw table every frame.
    std::vector<DrawData> drawScratch;
    std::unordered_map<const scene::Surface *, std::shared_ptr<Geometry>> geometries;
    // Finished immutable uploads can back a later transient mesh. Commands keep
    // their own references until their frame fence has completed.
    std::vector<std::shared_ptr<Geometry>> spareGeometry;
    uint64_t spareGeometryBytes = 0;

    ~Gpu();
    // Activation, or frame completion after scene base-buffer accounting.
    void publishStatistics();
    void createLayouts();
    void createTargets();
    void createPipelines(VkRenderPass output);
    void prepareFrame(VkCommandBuffer command, RasterRenderer &resources);
    void recordWorld(VkCommandBuffer command);
    void recordWater(VkCommandBuffer command);
    void recordViewModels(VkCommandBuffer command);
    void prepareViewModelDescriptors();
    void recordExposure(VkCommandBuffer command, RasterRenderer &resources, float seconds);
    bool castsLocalShadow(const Command &draw, uint32_t slot, bool staticCasters) const;
    void drawLocalCasters(VkCommandBuffer command, uint32_t slot, uint32_t face, bool staticCasters);
    void drawGeometry(VkCommandBuffer command, bool terrain = false, bool depthOnly = false);
    void drawSunCasters(VkCommandBuffer command, uint32_t cascade, bool staticCasters);
    void prepareSky(VkCommandBuffer command, RasterRenderer &resources);
    void display(VkCommandBuffer command, bool background);
    std::shared_ptr<Geometry> geometry(const scene::Draw &draw);
    void retireGeometry();
    std::shared_ptr<Geometry> takeGeometry(size_t vertexBytes, size_t indexBytes);
    const Bounds &frozenShadowBounds();
    VkDescriptorSet allocate(Frame &frame, VkDescriptorSetLayout layout);
    void ensure(std::unique_ptr<Buffer> &buffer, VkDeviceSize bytes, VkBufferUsageFlags usage,
                Buffer::Memory memory = Buffer::Memory::Host);
  };
} // namespace remix::forward
