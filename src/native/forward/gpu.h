#pragma once
#include "forward/bake/controller.h"
#include "forward/bindings.h"
#include "forward/fragment_statistics.h"
#include "forward/highlight_selection.h"
#include "forward/mixed/chart_highlights.h"
#include "forward/reflections.h"
#include "forward/ray/provider.h"
#include "forward/provider_plan.h"
#include "forward/probes/authoring.h"
#include "forward/probes/capture_providers.h"
#include "forward/probes/selection_upload.h"
#include "forward/probes/local_selection_preparation.h"
#include "forward/probe_batch/batch.h"
#include "forward/depth_batch/batch.h"
#include "forward/renderer.h"
#include "forward/resources.h"
#include "forward/shadows/maps.h"
#include "forward/shadows/moving_cache.h"
#include "forward/shadows/sun_maps.h"
#include "forward/water/pass.h"
#include "forward/viewmodel/target.h"
#include "platform/gpu_frames.h"
#include <chrono>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace remix::forward {
  struct Renderer::Gpu {
    static constexpr uint32_t materialTextureCount = 5;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDeviceSize maxStorageBufferRange = 0;
    uint32_t maxComputeGroupsX = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    Options options;
    techniques::Capabilities capabilities;
    techniques::ResourcePlan techniquePlan;
    scene::DisplayOutput displayOutput;
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
               unmappedLightmapPipeline = VK_NULL_HANDLE,
               depthPipeline = VK_NULL_HANDLE, depthLightmapPipeline = VK_NULL_HANDLE,
               shadowPipeline = VK_NULL_HANDLE, localShadowPipeline = VK_NULL_HANDLE,
               clusterPipeline = VK_NULL_HANDLE, compositePipeline = VK_NULL_HANDLE;
    VkPipeline indexedLightmapPipeline = VK_NULL_HANDLE;
    VkPipeline indexedRoughDielectricPipeline = VK_NULL_HANDLE;
    bool largeOpaqueTarget() const {
      return uint64_t(extent.width) * extent.height >= 3840ull * 2160;
    }
    VkPipeline depthBatchPipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE, surfaceSampler = VK_NULL_HANDLE,
              skySampler = VK_NULL_HANDLE;
    std::weak_ptr<scene::Texture> classifiedSkyTexture;
    bool skyTextureHasZeroRgb = false;
    VkRenderPass worldPass = VK_NULL_HANDLE, shadowPass = VK_NULL_HANDLE;
    VkRenderPass opaqueResumePass = VK_NULL_HANDLE;
    VkDescriptorSetLayout visibilitySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout visibilityLayout = VK_NULL_HANDLE, coveredClusterLayout = VK_NULL_HANDLE;
    VkPipeline coveredClusterPipeline = VK_NULL_HANDLE;
    VkPipeline tileDepthPipeline = VK_NULL_HANDLE, visibilityPipeline = VK_NULL_HANDLE;
    VkDescriptorPool visibilityPool = VK_NULL_HANDLE;
    VkFramebuffer worldFramebuffer = VK_NULL_HANDLE;
    std::unique_ptr<Image> color, depth, resolved;
    std::unique_ptr<shadows::SunMaps> sunMaps;
    std::unique_ptr<Image> normalGuide, reflectanceGuide;
    std::unique_ptr<Image> meterMaterial, resolvedMeterMaterial;
    std::unique_ptr<Reflections> reflections;
    std::unique_ptr<water::Pass> waterPass;
    std::unique_ptr<viewmodel::Target> viewModelTarget;
    std::unique_ptr<viewmodel::SunShadow> viewModelSun;
    std::unique_ptr<shadows::Maps> viewModelLocalMaps;
    VkPipeline viewModelPipeline = VK_NULL_HANDLE;
    VkPipeline viewModelEarlyPipeline = VK_NULL_HANDLE;
    VkPipeline giViewModelPipeline = VK_NULL_HANDLE, giViewModelEarlyPipeline = VK_NULL_HANDLE;
    std::unique_ptr<postfx::Renderer> postProcessing;
    VkDescriptorSetLayout postLayout = VK_NULL_HANDLE;
    ray::Provider rayLighting;
    VkDescriptorSetLayout giSceneLayout = VK_NULL_HANDLE;
    VkPipelineLayout giLayout = VK_NULL_HANDLE;
    VkPipeline giOpaquePipeline = VK_NULL_HANDLE, giLightmapPipeline = VK_NULL_HANDLE;
    VkPipeline terrainPipeline = VK_NULL_HANDLE, terrainLightmapPipeline = VK_NULL_HANDLE,
               terrainPartialLightmapPipeline = VK_NULL_HANDLE,
               giTerrainPipeline = VK_NULL_HANDLE, giTerrainLightmapPipeline = VK_NULL_HANDLE;
    std::shared_ptr<bake::Controller> baking;
    std::unique_ptr<probe_batch::Cache> probeVisibility;
    std::unique_ptr<styled_lighting::Cache> styledLighting;
    probes::CaptureProviders captureProviders;
    probes::CaptureSource reflectionSource() const {
      if (const auto source = captureProviders.standaloneSource(techniquePlan.capturePolicy.transport)) return *source;
      return baking->reflectionSource();
    }
    probes::SelectionFrames localSelections;
    std::unique_ptr<probes::LocalSelectionPreparation> localSelectionPreparation;
    std::optional<probes::CoveragePlanContext> localSelectionAttempt;
    std::weak_ptr<const probes::ImageAsset> localSelectionBaseline, localSelectionImage;
    void prepareLocalReflections(VkCommandBuffer command, RasterRenderer &resources);
    void prepareReflectionMasks(std::span<water::MaterialData> waterMaterials);
    std::optional<probes::Header> reflectionMaskHeader;
    float reflectionMaskBias = 0;
    uint64_t reflectionMaskRevision = 0;
    const probes::ImageAsset *reflectionImage() const;
    const probes::Header *reflectionHeader() const;
    const probes::LayerIdentities *reflectionIdentities() const;
    uint32_t reflectionLayer(uint64_t sceneId, uint64_t volumeId) const;
    probes::Descriptors reflectionDescriptor() const;
    std::unique_ptr<shadows::Maps> localMaps;
    shadows::Planner shadowPlanner;
    shadows::Selection shadowSelection;
    std::array<shadows::MovingCache, shadows::maximumLights> movingShadowCache;
    std::vector<shadows::MovingCaster> movingCasterScratch;
    HighlightSelection highlightSelection;
    std::weak_ptr<const mixed::Bindings> highlightSource;
    mixed::ChartHighlightWork chartHighlightWork;
    bool chartHighlightsActive = false;
    std::weak_ptr<const scene::BakeScene> staticBoundsSource;
    Bounds staticBounds;
    uint64_t baseTargetBytes = 0;

    struct Geometry {
      std::weak_ptr<scene::Surface> source;
      std::vector<Matrix> bones; // last uploaded pose, private to this frame slot
      std::unique_ptr<Buffer> vertices, indices;
      Bounds bounds;
      // One entry per resident mesh. Instances at different transforms may
      // replace it, while steady world meshes reuse it across frame slots.
      Bounds reflectionMaskBounds;
      uint64_t reflectionMaskRevision = 0;
      std::array<uint32_t, 2> reflectionMask{};
    };
    struct Command {
      std::shared_ptr<Geometry> geometry;
      const lightmap::Geometry::Entry *lightmap = nullptr;
      Bounds worldBounds;
      bool captured = false;
      bool cameraVisible = false;
      bool needsMaterial = true;
      bool unconditionalCoverage = false;
      bool opaqueDepth = false, depthBatched = false;
      scene::Draw source; // material, mesh and texture owners live through the fence
      VkDescriptorSet material = VK_NULL_HANDLE;
      uint64_t alphaSampler = 0;
      uint32_t index = 0, count = 0;
      uint32_t waterIndex = 0;
      std::array<uint32_t, 2> reflectionCandidates{};

      std::span<const lightmap::ReceiverRun> receiverRuns() const {
        return lightmap ? std::span(lightmap->receiverRuns) : std::span<const lightmap::ReceiverRun>{};
      }

      // Visibility and raster recording must enumerate the same ranges in
      // the same order. Split runs retain their own receiver coverage.
      template <typename Emit> void forEachIndexRange(bool splitReceivers, Emit emit) const {
        using Coverage = std::optional<lightmap::ReceiverCoverage>;
        const auto runs = splitReceivers ? receiverRuns() : std::span<const lightmap::ReceiverRun>{};
        if (!runs.empty()) {
          for (const auto &run : runs)
            emit(VkDrawIndexedIndirectCommand{run.indexCount, 1, run.firstIndex, 0, 0}, Coverage{run.coverage});
          return;
        }
        const VkDrawIndexedIndirectCommand range{lightmap ? lightmap->count : count, 1,
            lightmap ? 0 : source.firstIndex, lightmap ? 0 : source.vertexOffset, 0};
        emit(range, Coverage{});
      }
    };
    // One command buffer scope with a fixed pipeline layout. Create a fresh
    // cache when a pass starts or another recorder can change these bindings.
    struct DrawBindings {
      void bind(VkCommandBuffer command, VkPipelineLayout layout, VkDescriptorSet material,
                VkBuffer vertices, VkBuffer indices);

    private:
      VkDescriptorSet m_material = VK_NULL_HANDLE;
      VkBuffer m_vertices = VK_NULL_HANDLE, m_indices = VK_NULL_HANDLE;
    };
    struct Frame {
      VkDescriptorPool pool = VK_NULL_HANDLE;
      uint32_t poolCapacity = 0;
      VkDescriptorSet scene = VK_NULL_HANDLE, composite = VK_NULL_HANDLE;
      VkDescriptorSet viewModelScene = VK_NULL_HANDLE;
      ray::Provider::FrameInputs rayInputs;
      // One binding snapshot after provider publication, shared by all passes.
      // Provider frame owners retain the referenced images through this fence.
      probes::Descriptors reflectionProbes;
      VkDescriptorSet visibilitySet = VK_NULL_HANDLE;
      bool visibilityDescriptorsCurrent = false;
      size_t visibleOpaqueRanges = 0, visibleOpaqueBounds = 0;
      std::unique_ptr<Buffer> visibilityTiles, visibilityBounds, indirectDraws;
      std::unique_ptr<Buffer> constants, lights, draws, clusters, skyConstants;
      std::unique_ptr<Buffer> chartHighlightUpload;
      std::shared_ptr<const std::vector<uint32_t>> chartHighlightPayload;
      VkEvent chartHighlightCopy = VK_NULL_HANDLE;
      VkDeviceSize chartHighlightOffset = VK_WHOLE_SIZE;
      bool chartHighlightReady = false;
      std::shared_ptr<const probe_batch::Field> probeVisibility;
      std::shared_ptr<const styled_lighting::Publication> styledLighting;
      std::unique_ptr<depth_batch::Batch> depthBatch;
      std::unique_ptr<Buffer> viewModelConstants, viewModelReflectionConstants;
      Matrix viewModelProjection = scene::identity();
      bool viewModelStaticSunDirty = true;
      bool skyEnabled = false;
      VkDescriptorImageInfo skyImage{};
      std::vector<Command> commands, staticCommands;
      std::vector<Command> terrainCommands;
      std::vector<Command> waterCommands;
      std::vector<Command> viewModelCommands;
      std::vector<Command> viewModelCasters;
      std::array<std::vector<uint32_t>, shadows::maximumLights> staticLocalCasters;
      std::array<std::vector<uint32_t>, shadows::maximumLights> movingLocalCasters;
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
    std::vector<LightData> lightScratch, bakedLightScratch;
    std::vector<scene::SphereLight> sourceLightScratch;
    std::vector<LightRange> lightRangeScratch;
    std::vector<depth_batch::Source> depthSourceScratch;
    std::vector<Command *> depthDrawScratch;
    // Mesh surfaces submitted by one instance share an immutable pose vector.
    // The borrowed identity is used only within one frame's preparation.
    const std::vector<Matrix> *normalPoseSource = nullptr;
    std::vector<Matrix> poseNormals;
    struct VisibilityBounds {
      Vec3 minimum;
      uint32_t firstCommand;
      Vec3 maximum;
      uint32_t commandCount;
    };
    static_assert(sizeof(VisibilityBounds) == 32 && offsetof(VisibilityBounds, firstCommand) == 12 &&
                  offsetof(VisibilityBounds, commandCount) == 28);
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
    void recordChartHighlights(VkCommandBuffer command, Frame &frame, VkDeviceSize offset,
        const std::shared_ptr<const std::vector<uint32_t>> &payload);
    void prepareFrame(VkCommandBuffer command, RasterRenderer &resources, double seconds,
                      const lighting::Plan &lightingPlan);
    void prepareMaterialDescriptors(VkCommandBuffer command, RasterRenderer &resources, Frame &frame);
    void recordWorld(VkCommandBuffer command);
    void createOpaqueVisibility();
    uint32_t prepareOpaqueVisibility();
    void recordOpaqueVisibility(VkCommandBuffer command, uint32_t boundCount);
    void recordWater(VkCommandBuffer command);
    void recordViewModels(VkCommandBuffer command);
    void prepareViewModelDescriptors();
    void prepareDepthBatch(std::shared_ptr<const bake::Volume> source, std::span<const DrawData> materials);
    void recordExposure(VkCommandBuffer command, RasterRenderer &resources, float seconds);
    void drawLocalCasters(VkCommandBuffer command, uint32_t slot, uint32_t face, bool staticCasters);
    void drawGeometry(VkCommandBuffer command, bool terrain = false, bool depthOnly = false, bool indirect = false);
    void drawSunCasters(VkCommandBuffer command, uint32_t cascade, bool staticCasters);
    void submitShadowCaster(VkCommandBuffer command, const Command &draw, uint32_t layer, DrawBindings &bindings);
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
