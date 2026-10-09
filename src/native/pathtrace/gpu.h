#pragma once
#include "platform/memory_release.h"
#include <future>
#include "pathtrace/buffer_cache.h"
#include "pathtrace/frame_tables.h"
#include "pathtrace/renderer.h"
#include "pathtrace/shader_code.h"
#include "pathtrace/tlas_policy.h"
#include "platform/gpu_timers.h"
#include "platform/timing.h"
#include "platform/workers.h"
#include "scene/emission.h"
#include "scene/light_bvh.h"
#include "scene/opacity.h"
#include "scene/static_scene.h"
#include "scene/up_axis.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_set>

namespace remix::trace::detail {
  // Per-dispatch switches of the tracing stages (pathtrace.comp `features`),
  // pushed with each pipeline by Gpu::bindTrace.
  enum Feature : uint32_t {
    featureDecals = 1,
    featureRoulette = 2,
    featurePbrReuse = 4,
    featureAngularEmitters = 8,
    featureFirstOpaqueHit = 16,
    featureMultiSheetFallback = 32, // the composition pass composes layered pixels itself
  };
  // What a stage outside the lighting selection sees: decals on, the rest off.
  inline constexpr uint32_t defaultFeatures = featureDecals;
  inline constexpr uint32_t featuresOffset = 28; // after the composite's fragment constants
  inline constexpr VkPushConstantRange pushRanges[] = {{VK_SHADER_STAGE_FRAGMENT_BIT, 0, featuresOffset},
                                                       {VK_SHADER_STAGE_COMPUTE_BIT, featuresOffset, 4}};
  using namespace scene;

  inline void check(VkResult result) {
    if (result != VK_SUCCESS)
      throw result;
  }
  inline VkDeviceSize align(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
  }
  struct Image {
    VkDevice device = VK_NULL_HANDLE;
    VkImage handle = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ~Image() {
      if (view)
        vkDestroyImageView(device, view, nullptr);
      if (handle)
        vkDestroyImage(device, handle, nullptr);
      MemoryRelease::free(device, memory);
    }
  };
  struct Arena {
    std::unique_ptr<Buffer> buffer;
    std::map<VkDeviceSize, VkDeviceSize> free;
    void release(VkDeviceSize offset, VkDeviceSize size) {
      auto next = free.lower_bound(offset);
      if (next != free.begin()) {
        auto before = std::prev(next);
        if (before->first + before->second == offset) {
          offset = before->first;
          size += before->second;
          free.erase(before);
        }
      }
      if (next != free.end() && offset + size == next->first) {
        size += next->second;
        free.erase(next);
      }
      free.emplace(offset, size);
    }
  };
  struct Accel {
    VkDevice device;
    PFN_vkDestroyAccelerationStructureKHR destroy;
    std::shared_ptr<Arena> arena;
    VkDeviceSize offset = 0, size = 0;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    ~Accel() {
      if (handle)
        destroy(device, handle, nullptr);
      if (arena)
        arena->release(offset, size);
    }
  };
  struct TraceVertex {
    float position[4] = {};
    float uv[4] = {};
    float color[4] = {};
  };
  // TraceSurface: pathtrace/frame_tables.h.
  static_assert(sizeof(TraceVertex) == 48);
  struct Frame {
    Matrix inverseCamera, camera;
    float eye[4] = {}, direction[4] = {}, radiance[4] = {}; // radiance w: the sun's volumetric scale
    uint32_t config[4] = {}, target[4] = {};
    float jitter[4] = {};      // primary offset; z water reuse, w blue-noise sun/local bits
    float skyRadiance[4] = {}; // linear uniform environment; w: its texture + 1, -1 a uniform dome
    uint64_t sharcHash = 0, sharcAccumulation = 0, sharcResolved = 0, sharcCounters = 0;
    uint32_t sharcConfig[4] = {};
    float sharcGrid[4] = {}, sharcPreviousCamera[4] = {};
    float sun[4] = {}; // tangent of angular radius, SIGMA active, debug view, raw local-distance diagnostic
    uint64_t lightNodes = 0, lightEmitters = 0;
    uint32_t localConfig[4] = {};
    float localScale[4] = {};
    uint64_t lightGuideRead = 0, lightGuideWrite = 0;
    uint32_t lightGuideConfig[4] = {}; // capacity, node count, inverse cell size (float bits), reserved
    uint64_t torchNodes = 0, torchEmitters = 0, emissiveNodes = 0, emissiveEmitters = 0;
    uint32_t splitLocal[4] = {}; // mode, torch count, emissive count, debug view
    uint64_t reuseRead = 0, reuseWrite = 0;
    uint32_t reuseConfig[4] = {}; // previous frame valid, diagnostics, ray counter address low/high
    uint64_t indirectResults = 0, blueNoise = 0;
    float firefly[4] = {};       // outgoing local direct / indirect luminance limits; reserved
    uint32_t pathConfig[4] = {}; // adaptive termination, minimum completed bounces, sheet flags (1 traced indirect, 2 local lights), secondary ray mask
    float domeTransform[3][4] = {};
    uint64_t visibilityNodes = 0, visibilityLeaves = 0, visibilityTable = 0;
    uint64_t movedLights = 0; // scene::MovedLight pairs, count in visibilityConfig[1]
    uint32_t visibilityConfig[4] = {}; // table row words, moved light count, light-tree nodes in the table, reserved
    float visibilityProbe[4] = {};     // world units: probe offset above the surface, decal attachment distance; texture footprint scale (render/output size)
    uint64_t restirCurrent = 0, restirHistoryRead = 0, restirHistoryWrite = 0, restirRemap = 0;
    uint32_t restirConfig[4] = {}; // candidates, spatial samples, temporal M cap, flags (1 initial visibility)
    float restirParams[4] = {};    // spatial radius (pixels), emitter count, reserved
    uint64_t regirRead = 0, regirWrite = 0, regirSlots = 0;
    // Caustics (visibility.glsl causticTint): strength (8 bits) and contrast
    // (8 bits, in 32nds), then the half float focus depth in world units
    // (bits 16-31); 0 off.
    uint64_t caustics = 0;
    uint32_t regirConfig[4] = {}; // cell capacity, slots per cell, cells per world unit (float bits), active
    // lightCookies: each light cookie slot's texture in this frame's table + 1.
    uint64_t orcaTable = 0, lightCookies = 0;
    uint32_t orcaConfig[4] = {}; // entry capacity, sparse tile size, voxel levels, active
    float orcaParams[4] = {};    // smallest voxel (world units), level growth, minimum samples, maximum relative error
    // remixapi_InstanceInfoMaskEXT CUT. cutConfig: first coverage texture + 1
    // (0: nothing cuts), coverage textures. cutSlots: four bits per mask
    // group (1..32), its coverage texture (from the first) + 1 or 0.
    uint32_t cutConfig[4] = {}, cutSlots[4] = {};
    // remixapi_InstanceInfoCameraCullingEXT. cameraNodes: the visibility
    // partition's nodes; cameraLeaves: a bit per leaf, set where the camera
    // sees the leaf (both zero unless visibility culling is active).
    // cameraCull: active flags (1 back faces, 2 visibility), the distance
    // before a surface at which a camera ray is located (float bits).
    uint64_t cameraNodes = 0, cameraLeaves = 0;
    uint32_t cameraCull[4] = {};
    // Sparse rendering (sparse.cpp): the selected pixels, their count and
    // indirect dispatch. sparseConfig: active, threshold levels passed of 256.
    uint64_t sparseList = 0, sparseCount = 0;
    uint32_t sparseConfig[4] = {};
    // Volumetrics (volume.cpp, volume.glsl). volumeConfig: active, cells per
    // side, levels, update interval. volumeFrame: update counter, history
    // samples, march steps, reset. volumeParams: smallest cell (world units),
    // march distance, scattered light scale, base height. volumeMedium:
    // scattering albedo, extinction per world unit. volumeHeight: up axis,
    // density falloff per world unit. volumeEye/Previous: the windows' centre.
    uint64_t volumeGrid = 0;
    // Seconds of the runtime's clock (wrapped), for scrolling maps; the world's
    // up axis (0 x, 1 y, 2 z: rtx.native.pathtrace.volumetricUpAxis).
    float animationTime = 0, upAxis = 2;
    uint32_t volumeConfig[4] = {}, volumeFrame[4] = {};
    float volumeParams[4] = {}, volumeMedium[4] = {}, volumeHeight[4] = {};
    float volumeEye[4] = {}, volumePrevious[4] = {};
    // remixapi_CameraMediumEXT: transmittance colour, measurement distance (0: in air).
    float cameraMedium[4] = {};
    uint64_t sheetResults = 0; // SheetResults: the sheet lighting pass's transport
    // Sky guiding (sky_guiding.cpp, sky_guiding.glsl): each world cell's
    // learned sky directions, last frame's (read, 0: none yet) and this
    // frame's. skyGuideConfig: slot capacity, cells per world unit (float
    // bits), guided share of the extra sky rays (float bits), active.
    uint64_t skyGuideRead = 0;
    uint64_t skyGuideWrite = 0;
    // IES profiles (scene/ies.h): kIesBins floats each, a sphere emitter's
    // corners[2].x the profile + 1.
    uint64_t iesProfiles = 0;
    uint32_t skyGuideConfig[4] = {};
    // Camera rays (pathtrace.comp ray()): inverseCamera relative to the eye,
    // which the shader adds back. In single precision the absolute matrix
    // holds the eye's translation scaled by the inverse near plane, and loses
    // the ray directions to rounding far from the origin (near geometry such
    // as a view model jitters as the camera moves).
    Matrix inverseCameraRelative{};
    // Local occlusion probes (pathtrace.comp, localOcclusionConfidence): per
    // pixel, the arrived share of its local light, last frame's (0: none)
    // and this frame's. localProbeParams: movingShadowBlur, reserved.
    uint64_t localProbeRead = 0, localProbeWrite = 0;
    float localProbeParams[4] = {};
    // Volumetrics: each source's share of the scattered light (sun, lights,
    // emissive surfaces, sky; the volumetric*Scale options).
    float volumeLightScales[4] = {1, 1, 1, 1};
    // The NUMOS sky's cloud shadow (sky::Renderer::cloudShadow): rows taking
    // a world position (w 1) to (u, v, slice) in its box; cloudShadowConfig:
    // its texture + 1 (0: none), slices.
    float cloudShadow[3][4] = {};
    uint32_t cloudShadowConfig[4] = {};
    // A bit per texture slot (slot / 32, its bit slot % 32): the slot holds
    // sRGB-encoded texels (stored UNORM), decoded with pow 2.2 (scene.glsl).
    uint32_t srgbTextures[32][4] = {};
  };
  static_assert(sizeof(Frame) == 1792 && offsetof(Frame, srgbTextures) == 1280 &&
                offsetof(Frame, localProbeRead) == 1168 &&
                offsetof(Frame, localProbeParams) == 1184 && offsetof(Frame, volumeLightScales) == 1200 &&
                offsetof(Frame, cloudShadow) == 1216 && offsetof(Frame, cloudShadowConfig) == 1264 &&
                offsetof(Frame, inverseCameraRelative) == 1104 &&
                offsetof(Frame, skyGuideRead) == 1064 && offsetof(Frame, skyGuideConfig) == 1088 && offsetof(Frame, volumeGrid) == 912 && offsetof(Frame, volumePrevious) == 1024 && offsetof(Frame, sparseList) == 880 && offsetof(Frame, sparseConfig) == 896 && offsetof(Frame, cameraCull) == 864 && offsetof(Frame, cutConfig) == 816 && offsetof(Frame, orcaParams) == 800 && offsetof(Frame, regirConfig) == 752 && offsetof(Frame, restirCurrent) == 656 && offsetof(Frame, visibilityNodes) == 592 && offsetof(Frame, pathConfig) == 528 &&
                offsetof(Frame, firefly) == 512 && offsetof(Frame, indirectResults) == 496 &&
                offsetof(Frame, lightGuideRead) == 384 && offsetof(Frame, lightGuideConfig) == 400 &&
                offsetof(Frame, lightNodes) == 336 && offsetof(Frame, localConfig) == 352 &&
                offsetof(Frame, localScale) == 368 && offsetof(Frame, skyRadiance) == 224 &&
                offsetof(Frame, sharcHash) == 240 && offsetof(Frame, sharcConfig) == 272 &&
                offsetof(Frame, sharcGrid) == 288 && offsetof(Frame, sharcPreviousCamera) == 304 &&
                offsetof(Frame, sun) == 320);
  inline void barrier(VkCommandBuffer command, VkPipelineStageFlags source, VkAccessFlags writes,
                      VkPipelineStageFlags destination, VkAccessFlags reads) {
    VkMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = writes, .dstAccessMask = reads};
    vkCmdPipelineBarrier(command, source, destination, 0, 1, &b, 0, nullptr, 0, nullptr);
  }
  constexpr VkBufferUsageFlags inputUsage =
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  constexpr VkPipelineStageFlags buildStage = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  constexpr VkAccessFlags asRead = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  constexpr VkAccessFlags asWrite = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
} // namespace remix::trace::detail

namespace remix::sky {
  class Renderer;
}
namespace remix {
  struct PathTracer::Gpu {
    remixapi_FramePerformanceEXT performance{.structSize = sizeof(remixapi_FramePerformanceEXT)};
    remixapi_FrameBreakdownEXT breakdown{.structSize = sizeof(remixapi_FrameBreakdownEXT)};
    uint64_t materialTicks = 0, temporalTicks = 0;
    // REMIX_FRAME_DIGEST: address-free hash of the prepared GPU inputs.
    const bool digestEnabled = std::getenv("REMIX_FRAME_DIGEST") != nullptr;
    uint64_t digest = 0;
    std::vector<std::shared_ptr<trace::detail::BufferMemory>> bufferMemory;
    // The next block of each memory type in use, allocated ahead on another
    // thread: allocating device memory takes the driver tens of milliseconds
    // here, which a frame that needed a new block (a new mesh's acceleration
    // structure, a grown table) used to wait for.
    static constexpr VkDeviceSize bufferBlockSize = 32 * 1024 * 1024;
    std::vector<std::pair<uint32_t, std::future<std::shared_ptr<trace::detail::BufferMemory>>>> spareBufferMemory;
    // A spare is not allocated the moment its predecessor is taken: the driver
    // serializes its memory calls, and the frames that fill a block are the
    // ones creating buffers, which then waited behind the allocation (20 ms
    // and more, on the render thread). It is asked for here and allocated in
    // the first frame that follows one without a new buffer (launchSpares).
    struct SpareWanted {
      uint32_t type = 0;
      bool host = false;
      uint32_t frames = 0;
    };
    std::vector<SpareWanted> sparesWanted;
    uint64_t buffersCreated = 0, buffersSeen = 0;
    void launchSpares();
    // `warm`: also pays the driver's first-use costs (for a spare, off the render thread).
    static std::shared_ptr<trace::detail::BufferMemory> allocateBufferMemory(VkDevice device, VkDeviceSize size,
                                                                           uint32_t type, bool host, bool warm);
    using Buffer = trace::detail::Buffer;
    using Image = trace::detail::Image;
    using Arena = trace::detail::Arena;
    using Accel = trace::detail::Accel;
    using TraceVertex = trace::detail::TraceVertex;
    using TraceSurface = trace::detail::TraceSurface;
    using Frame = trace::detail::Frame;

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory = {};
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    PFN_vkCreateAccelerationStructureKHR createAs = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAs = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAsAddress = nullptr;
    PFN_vkGetBufferDeviceAddress getBufferAddress = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR buildSizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR buildAs = nullptr;
    PFN_vkCmdCopyAccelerationStructureKHR copyAs = nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR writeAsProperties = nullptr;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline transparencyCompute{}, temporalTransparencyCompute{};
    VkPipeline singleTransparencyCompute{}, singleTemporalTransparencyCompute{}, frontGuidesCompute{}, psrCompute{};
    // The front sheet's lighting (SHEET_LIGHTING_PASS) for the single-sheet
    // composition pass, and its transport (16 bytes a pixel).
    VkPipeline sheetCompute{}, sheetTemporalCompute{};
    bool multiSheetFailed = false; // the multi-sheet pass's compile failed: the composition pass composes layers
    std::unique_ptr<Buffer> sheetResults;
    VkPipeline compute = VK_NULL_HANDLE, composite[2] = {};
    VkPipeline splitDirectCompute = VK_NULL_HANDLE;
    struct IndirectPass {
      std::unique_ptr<Buffer> results;
      VkPipeline pipeline = VK_NULL_HANDLE;
      bool active = false, failed = false;
      // 2: the indirect bounces trace in their own pass (the default: the
      // opaque pass and it each carry one copy of the light sampler, and
      // compile in half the time of the combined kernel). 1: only for combined
      // lighting without SHARC queries (REMIX_SPLIT_INDIRECT=auto). 0: off (=0).
      uint32_t mode = [] {
        const auto *value = std::getenv("REMIX_SPLIT_INDIRECT");
        return !value ? 2u : std::strcmp(value, "0") == 0 ? 0u : std::strcmp(value, "auto") == 0 ? 1u : 2u;
      }();
      const char *modeFile = std::getenv("REMIX_INDIRECT_MODE_FILE");
      uint64_t controlReset = UINT64_MAX;
    } indirectPass;
    void configureIndirect(Frame &frame, const TraceOptions &options, bool querySharc);
    // Whether the separate indirect pass is built for these options.
    bool indirectPlanned(const TraceOptions &options) const {
      return indirectPass.mode != 0 && !indirectPass.failed && options.bounces != 0 && !options.sparseRendering;
    }
    void configureProfile(const TraceOptions &options);
    void destroyIndirect();
    // A lighting pipeline's variants (selectLightingPipeline), keyed by the
    // variant bits; the pair's bool: a request was made (its result pending
    // while the pipeline is null).
    struct LightingPipeline {
      const uint32_t *code = nullptr;
      size_t bytes = 0;
      VkPipelineLayout layout = VK_NULL_HANDLE;
      std::map<uint32_t, std::pair<VkPipeline, bool>> specialized;
      bool supportsGuiding = true;
      bool supportsRoulette = true;
      bool preferFirstHit = true;
      bool splitDirect = false;  // the split direct pass: combined-mode variants
      bool baseEarlyOut = false; // the source's shadowEarlyOut specialization
      bool baseSeparateIndirect = false; // the source consumes the separate indirect pass
      bool variants = true;      // selected through selectLightingPipeline: variants prewarmed
    };
    std::unordered_map<VkPipeline, LightingPipeline> lightingPipelines;
    // Variant key bits (selectedLightingKey). Sparse launch mode occupies the
    // two bits from variantSparseShift.
    enum VariantBit : uint32_t {
      variantGuided = 1,
      variantSmallSet = 2,
      variantResampling = 4,
      variantSplit = 8,
      variantCombinedDirect = 16,
      variantLocal = 64,
      variantProfile = 256,
      variantSeparateIndirect = 1024,
      variantEarlyOut = 8192,
      variantSparseShift = 14,
    };
    // The options the lighting pipelines were last configured with
    // (beginFrame): the settings the scene variants are prewarmed for.
    TraceOptions lightingOptions{};
    bool prewarmPending = false; // a variant setting changed: prewarm after the frame's requests
    // On a SHARC-capable device the cache-querying stages are the only
    // lighting family (lightingStage): the query is gated per frame, the
    // registers are the same (168 measured) and the compiles halve.
    bool cachedStages = false;
    void querySharcSupport(); // sets sharc.stats.supported and cachedStages
    trace::Stage lightingStage(trace::Stage uncached) const;
    std::vector<LightingPipeline> deferredVariants; // sources whose variants follow the deferred batches
    // Compiles (or, prefetching, schedules) the source's variant for `key`.
    VkResult requestLightingVariant(LightingPipeline &source, uint32_t key, VkPipeline *out, bool prefetch);
    // Schedules the variants the scene can select for this source under the
    // current settings, so gameplay never waits for a compile: with and
    // without local lights, the small set, resampling, guiding and split
    // lighting.
    void prewarmLightingVariants(LightingPipeline &source);
    struct RayProfile {
      std::unique_ptr<Buffer> counters, readback;
      bool pending = false;
      const char *path = std::getenv("REMIX_RAY_PROFILE");
    } rayProfile;
    void beginRayProfile(VkCommandBuffer command, Frame &frame);
    void finishRayProfile(VkCommandBuffer command);
    void completeRayProfile();
    struct PbrReuse {
      std::unique_ptr<Buffer> history[2], readback;
      uint32_t writeIndex = 0;
      uint64_t signature = 0;
      bool active = false, valid = false, pending = false, failed = false;
      const char *diagnostics = std::getenv("REMIX_PBR_REUSE_DIAGNOSTICS");
    } pbrReuse;
    void configurePbrReuse(VkCommandBuffer command, Frame &frame, const TraceOptions &options,
                           uint64_t signature, bool stable);
    void completePbrReuse();
    void finishPbrReuse(VkCommandBuffer command);
    bool localFirstHitActive = false;
    bool decalMaterialsActive = false;
    bool shadowEarlyOutActive = false; // rtx.native.pathtrace.shadowEarlyOut
    bool localIlluminationActive = false;
    bool localResamplingActive = false;
    bool localAngularActive = false;
    bool localGuidingActive = false;
    bool rouletteActive = false, rouletteFailed = false;
    // sparse: the launch of the opaque pass under sparse rendering (1 the
    // selected pixels, 2 the others); null until that variant has compiled.
    VkPipeline selectLightingPipeline(VkPipeline pipeline, bool combinedDirect = false,
                                      bool separateIndirect = false, uint32_t sparse = 0);
    void forgetLightingPipeline(VkPipeline pipeline);
    VkShaderModule shaders[3] = {};
    VkQueryPool compaction = VK_NULL_HANDLE, timestamps = VK_NULL_HANDLE;
    uint32_t compactionCapacity = 0; // queries in `compaction`
    uint32_t timestampBits = 0;
    const char *stageProfile = std::getenv("REMIX_STACK_PROFILE");
    const char *stateProfile = stageProfile ? std::getenv("REMIX_TRACE_STATE") : nullptr;
    uint32_t selectedLightingKey = 0;
    // The selected pipeline's per-dispatch switches and layout (bindTrace).
    uint32_t selectedFeatures = trace::detail::defaultFeatures;
    VkPipelineLayout selectedLayout = VK_NULL_HANDLE;
    void bindTrace(VkCommandBuffer command, VkPipeline pipeline, VkPipelineLayout pipelineLayout, uint32_t features);
    // Binds the pipeline selectLightingPipeline just returned.
    void bindSelected(VkCommandBuffer command, VkPipeline pipeline) {
      bindTrace(command, pipeline, selectedLayout, selectedFeatures);
    }
    std::string traceState;
    // The tables every ray and light sample reads (surface table, light tree
    // and emitters, potentially visible light table, frame constants) are
    // kept in device memory and filled through staging copies of what
    // changed. In host-visible memory, which is system RAM on discrete GPUs,
    // each read crosses the bus: slower on average and by a different amount
    // every frame. REMIX_DEVICE_LOCAL_SCENE=0 keeps the host placement for
    // comparison.
    bool deviceLocalScene = [] {
      const auto *value = std::getenv("REMIX_DEVICE_LOCAL_SCENE");
      return !value || std::strcmp(value, "0") != 0;
    }();
    std::unique_ptr<Buffer> tableUpload;
    // What this slot's device-local surface table holds: only the blocks
    // that differ from it are copied again.
    std::vector<uint8_t> tableShadow;
    // Diagnostic replay must measure fresh lighting even after a static view
    // converges. Keep camera/history/cache contents intact and only bypass the
    // final-image reuse shortcut. Ordinary rendering retains that shortcut.
    const bool profileEveryFrame = [] {
      const char *value = std::getenv("REMIX_PROFILE_EVERY_FRAME");
      return value && std::strcmp(value, "1") == 0;
    }();
    bool timingPending = false, imagesInitialized = false;
    uint32_t debugView = 0;
    bool falseColorView = false; // the debug view in false colour: shown without the tone curve
    PathTracer::DisplayOutput display;
    VkExtent2D extent = {}, displayExtent = {};
    VkRenderPass displayPass{};
    VkFormat displayDepthFormat{};
    VkImageView displayDepth{};
    std::shared_ptr<upscale::Ngx> ngx;
    upscale::Mode requestedUpscaler = upscale::Mode::Off;
    upscale::Backend requestedBackend = upscale::Backend::Native;
    bool rrRequested = true;
    // Ray Reconstruction's requested model (true: CNN) and preset (0: the
    // model's own; else NGX's 1-6), the preset created and why the requested
    // model is not the loaded one.
    bool rrCnnRequested = false;
    uint32_t rrPresetRequested = 0, rrPresetCreated = 0;
    std::string rrModelNote;
    uint32_t failedBackends = 0;
    uint64_t reconstructionFailures = 0, reconstructedFrames = 0, reconstructionResets = 0;
    uint32_t reconstructionWarmFrames = 0;
    bool rrActive() const { return upscaler && upscaler->statistics().backend == upscale::Backend::DlssRr; }
    TraceOptions effectiveOptions(TraceOptions options) const;
    upscale::Backend selectBackend() const;
    uint32_t fsrQuality = 1;
    std::unique_ptr<upscale::Renderer> upscaler;
    std::unique_ptr<upscale::NeuralUplift> neuralUplift;
    std::unique_ptr<Image> tonemapTarget, tonemapDepth;
    VkRenderPass tonemapPass = VK_NULL_HANDLE;
    VkFramebuffer tonemapFramebuffer = VK_NULL_HANDLE;
    VkPipeline tonemapComposite[2] = {};
    VkDescriptorSetLayout blitSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool blitPool = VK_NULL_HANDLE;
    VkDescriptorSet blitSet = VK_NULL_HANDLE;
    VkPipelineLayout blitLayout = VK_NULL_HANDLE;
    VkPipeline blitPipeline = VK_NULL_HANDLE;
    VkShaderModule blitFragModule = VK_NULL_HANDLE;
    bool neuralUpliftRan = false, neuralUpliftValid = false;
    std::unique_ptr<Image> depthImage(VkFormat format, VkExtent2D size = {});
    void createTonemapAndBlit(VkRenderPass pass, VkFormat depthFormat);
    void destroyTonemapAndBlit();
    TraceOptions currentTraceOptions{};
    std::string fsrFailure;
    void createTarget();
    std::unique_ptr<Image> world, transmission, sky;
    std::unique_ptr<postfx::Renderer> postFallback;
    postfx::Renderer *postProcessing = nullptr;
    bool retainPostExposure = false;
    VkDescriptorSetLayout postSetLayout{};
    using PrimaryDraw = trace::PrimaryDraw;
    struct PrimaryPush {
      scene::Matrix model;
      float jitter[2] = {};
      uint32_t surfaceIndex = 0, stableDepth = 0;
      std::array<float, 4> reverseDepth{};
    };
    static_assert(sizeof(PrimaryPush) == 96);
    std::array<float, 4> primaryReverseDepth{};
    // The frame's primary draws (the frame tables' static prefix, then the rest).
    size_t primaryCount() const { return tables.primaryCount(); }
    const PrimaryDraw &primaryDraw(size_t i) const { return tables.primary(i); }
    std::unique_ptr<Image> primary[7];
    VkRenderPass primaryPass[2] = {};
    VkFramebuffer primaryFramebuffer[2] = {};
    VkPipeline primaryPipeline[2] = {};
    VkPipelineLayout primaryLayout{};
    bool primaryInitialized = false;
    void createPrimary(VkFormat depthFormat, VkImageView depthView);
    void destroyPrimary();
    void recordPrimary(VkCommandBuffer command, bool rasterPrimary);
    // Large primary passes record secondary command buffers on the workers:
    // one transient pool per task and frame slot, reset after the slot's fence.
    struct PrimaryRecording {
      std::vector<VkCommandPool> pools;
      std::vector<VkCommandBuffer> buffers; // two per pool: primary passes 0 and 1
      VkCommandBuffer viewModel[2] = {};    // the view model's draws (from the first pool)
      bool reset = false;                   // pools reset for this frame
    } primaryRecording;
    void destroyPrimaryRecording(PrimaryRecording &recording);
    VkRenderPass skyPass{};
    // The NUMOS sky (sky/renderer.h), created when first enabled. `skyReady`:
    // it has outputs from an earlier frame; `skyApplied`: this frame's sky
    // and sun are NUMOS's; `skyActive`: its work for this frame is recorded.
    std::shared_ptr<sky::Renderer> numosSky;
    config::SkySettings skySettings;
    config::SkyControl skyControl;
    // The sky's last frame, for the API's thread.
    mutable std::mutex skyStateMutex;
    config::SkyState skyState;
    // skyFailure: the error that stopped the sky (it stays off until the
    // sky settings change).
    std::string skyUnavailable, skyReason, skyFailure;
    bool skyReady = false, skyApplied = false, skyActive = false;
    VkFramebuffer skyFramebuffer{};
    uint32_t queueFamily{};
    std::unique_ptr<denoise::Renderer> denoiser;
    VkPipeline temporalCompute{}, confidenceCompute{};
    VkPipelineLayout temporalLayout{};
    // The world's up axis for volumetricUpAxis Auto.
    scene::UpAxisDetector upAxis;
    uint64_t shadowFrames = 0, shadowResets = 0;
    bool sigmaCapable = false, localCapable[2] = {};
    bool postSigma = false, postSigmaRequested = false;
    bool localSeen = false, postLocal = false, postLocalRequested = false;
    // Emissive (surface) lights were present: only then is local lighting
    // split by family or into a separate pass (API "torch" lights alone use
    // the combined local signal).
    bool emissiveSeen = false;
    uint32_t emissiveHold = 0; // frames the emissive estimator outlasts the last emissive light
    bool postLocalDenoise = false, postLocalDenoiseRequested = false;

    float previousLocalIntensity = 0, previousLocalRange = 0;
    uint32_t previousLocalAdaptive = 0, localResponsiveFrames = 0;
    bool previousLocalBlueNoise = false;
    float previousSun[9] = {}, pendingSun[9] = {};
    std::unique_ptr<Buffer> blueNoise;
    bool blueNoiseFailed = false;
    void configureBlueNoise(VkCommandBuffer command, Frame &frame, const TraceOptions &options);
    bool postReblur = false, postTaa = false, postAttempted = false, rendered = false;
    std::string postFailure;
    remixapi_DenoisingCapabilitiesEXT postCapabilities{};
    scene::TemporalHistory history;
    using VertexHistory = scene::TemporalTable<std::shared_ptr<Buffer>>;
    VertexHistory previousVertices, pendingVertices, retiredPreviousVertices;
    scene::Matrix previousCamera{}, pendingCamera{}, previousView{}, previousProjection{},
        pendingProjection{};
    float previousJitter[2] = {};
    PathTracer::FrameGenerationInputs frameGeneration; // of the frame being prepared
    denoise::Frame temporalFrame{};
    uint32_t previousSeed{};
    std::chrono::steady_clock::time_point previousTime{}, pendingTime{};
    uint64_t historyResets = 0, denoisedFrames = 0, postPeakMemory = 0;
    std::vector<std::shared_ptr<Arena>> arenas;
    std::unique_ptr<Buffer> scratch, table, uniform;
    scene::LightBvh lightBvh;
    std::vector<scene::SphereLight> lightInput; // prepareScene's gathered lights (storage kept)
    scene::LightVisibility lightVisibility;
    std::unique_ptr<Buffer> visibilityNodes, visibilityLeaves, visibilityTable;
    uint64_t visibilityPartitionVersion = 0, visibilityTableVersion = 0;
    // remixapi_InstanceInfoCameraCullingEXT: the partition's nodes and the
    // leaves the camera sees (a bit per leaf), replaced when the partition or
    // the camera's cluster changes; shared by the frame slots like the above.
    struct CameraCulling {
      std::unique_ptr<Buffer> nodes, leaves;
      uint64_t nodesVersion = 0, leavesVersion = 0; // partition versions held (0: none)
      int32_t leavesClusters[2] = {-1, -1};         // the clusters whose united set `leaves` holds
      uint64_t lastVersion = 0;                     // previous traced frame: partition and
      int32_t lastCluster = -1;                     // camera cluster
      uint32_t visibleClusters = 0, visibleLeaves = 0; // of `leaves`
      remixapi_CameraCullingStatisticsEXT statistics{.structSize = sizeof(remixapi_CameraCullingStatisticsEXT)};
    } cameraCulling;
    // Sets the frame's camera culling constants; uploads what they refer to.
    void configureCameraCulling(VkCommandBuffer command, Frame &frame, const TraceOptions &options,
                                const RasterRenderer &resources);
    // remixapi_LocalLightSetStatisticsEXT: table uploads; instances with
    // emitters this frame; CPU time of the frame's light update (hierarchies
    // and visibility table), last and longest of the last 300 to 600 frames.
    uint64_t visibilityTableUploads = 0;
    uint32_t emittingInstances = 0, dynamicEmittingInstances = 0;
    double lightUpdateMilliseconds = 0, lightUpdatePeak[2] = {};
    uint32_t lightUpdatePeakFrames = 0;
    scene::EmissionHistory emissionHistory;
    scene::LightBvh torchBvh, emissiveBvh;
    std::unique_ptr<Buffer> torchNodes, torchEmitters, emissiveNodes, emissiveEmitters;
    uint64_t previousFamilyGeneration[2] = {};
    uint32_t familyResponsive[2] = {};
    bool splitLightingActive = false, postSplit = false, postSplitRequested = false;
    bool postCombinedDirect = false;
    std::unique_ptr<Buffer> lightNodes, lightEmitters;
    std::unique_ptr<Buffer> movedLights; // this frame's scene::LightBvh::moved()
    // The IES profiles (NativeResources::iesProfiles) the buffer holds.
    std::shared_ptr<const std::vector<float>> iesUploaded;
    std::shared_ptr<Buffer> iesBuffer;
    std::unique_ptr<Buffer> lightCookies; // this frame's cookie table (Frame::lightCookies)
    uint64_t lightBufferGeneration = 0, torchBufferGeneration = 0, emissiveBufferGeneration = 0;
    // The hierarchy (LightBvh::instance) each slot's buffers hold; with the
    // generation above, what an upload of only the changes builds on.
    uint64_t lightBufferInstance = 0, torchBufferInstance = 0, emissiveBufferInstance = 0;
    scene::LightBvh::Ranges changedEmitters, changedNodes; // upload scratch
    std::vector<uint8_t> lightUploadPacking;
    struct LightGuiding {
      static constexpr uint32_t capacity = 65536;
      std::unique_ptr<Buffer> history[2], readback;
      uint32_t writeIndex = 0;
      bool valid = false, pending = false, attempted = false, pipelineFailed = false;
      uint64_t generation = 0;
      float range = 0, cellSize = 2;
      bool resampled = false;
      bool split = false;
      remixapi_LocalLightGuidingStatisticsEXT stats{.structSize =
                                                        sizeof(remixapi_LocalLightGuidingStatisticsEXT)};
    } lightGuiding;
    void configureLightGuiding(Frame &frame, const TraceOptions &options);
    // Sky guiding (sky_guiding.cpp): per world cell and facing, a histogram
    // of the directions its first-surface sky rays found the sky in, trained
    // this frame and read the next (two banks, like lightGuiding).
    struct SkyGuiding {
      static constexpr uint32_t capacity = 16384, stride = 68; // words per slot: key, total, 2 reserved, 64 bins
      std::unique_ptr<Buffer> history[2];
      uint32_t writeIndex = 0;
      bool valid = false, attempted = false, active = false;
      float cellSize = 0;
    } skyGuiding;
    void configureSkyGuiding(Frame &frame, const TraceOptions &options);
    void beginSkyGuiding(VkCommandBuffer command);
    // ReSTIR DI (restir.cpp, restir.glsl).
    struct Restir {
      VkPipeline initial = VK_NULL_HANDLE, spatial = VK_NULL_HANDLE;
      std::unique_ptr<Buffer> current, history[2], remap;
      uint32_t writeIndex = 0, width = 0, height = 0;
      bool historyValid = false, active = false;
      // Emitter identities of the previous frame, for the index remap.
      std::vector<std::pair<uint64_t, uint32_t>> keys;
    } restir;
    void configureRestir(Frame &frame, const TraceOptions &options, VkExtent2D extent);
    // Local occlusion probes (restir.cpp; pathtrace.comp, localOcclusionConfidence):
    // last frame's and this frame's, ping-pong.
    struct LocalProbes {
      std::unique_ptr<Buffer> buffers[2];
      uint32_t writeIndex = 0, width = 0, height = 0;
      bool valid = false;
    } localProbes;
    void configureLocalProbes(Frame &frame, const TraceOptions &options, VkExtent2D extent);
    // Presampled light pools per world cell (regir.cpp, regir.glsl).
    struct Regir {
      static constexpr uint32_t capacity = 1u << 15, slots = 64;
      VkPipeline presample = VK_NULL_HANDLE;
      std::unique_ptr<Buffer> cells[2], pool;
      uint32_t writeIndex = 0;
      bool valid = false, active = false, pipelineFailed = false;
    } regir;
    void configureRegir(Frame &frame, const TraceOptions &options);
    // Sparse rendering (sparse.cpp): this frame's selected pixels, listed by
    // a compaction pass for the indirect launch of the opaque lighting pass.
    struct Sparse {
      VkPipeline compact = VK_NULL_HANDLE, arguments = VK_NULL_HANDLE;
      std::unique_ptr<Buffer> list, count;
      bool active = false, pipelineFailed = false;
      // Launched sparse this frame (both opaque variants had compiled).
      bool launched = false;
      float rate = 1;         // selected share of the pixels this frame
      float launchedRate = 1; // the share last launched with (1: dense)
      const char *inactiveReason = "not requested";
    } sparse;
    void configureSparse(VkCommandBuffer command, Frame &frame, const TraceOptions &options, VkExtent2D extent);
    // Volumetrics (volume.cpp): the medium's scattered light in nested
    // world-space grids, one buffer for every level's cells.
    struct Volume {
      VkPipeline update = VK_NULL_HANDLE;
      std::unique_ptr<Buffer> cells;
      bool active = false, pipelineFailed = false;
      bool valid = false; // the cells belong to `eye` and the sizes below
      uint32_t cellsPerSide = 0, levels = 0, counter = 0;
      float cellUnits = 0, eye[3] = {};
      VkDeviceSize bytes = 0; // requested size of `cells`
      const char *inactiveReason = "not requested";
    } volume;
    void configureVolume(VkCommandBuffer command, Frame &frame, const TraceOptions &options);
    void recordVolume(VkCommandBuffer command);
    void recordSparse(VkCommandBuffer command, VkExtent2D extent);
    // Per-frame radiance cache from sparse paths (orca.cpp, orca.glsl).
    struct Orca {
      static constexpr uint32_t capacity = 1u << 18;
      VkPipeline sparse = VK_NULL_HANDLE;
      std::unique_ptr<Buffer> table;
      bool active = false, pipelineFailed = false;
      uint32_t tile = 4;
    } orca;
    void configureOrca(Frame &frame, const TraceOptions &options);
    void recordOrca(VkCommandBuffer command, VkExtent2D extent);
    void recordRegir(VkCommandBuffer command);
    void recordRestir(VkCommandBuffer command, VkExtent2D extent);
    void beginLightGuiding(VkCommandBuffer command);
    void finishLightGuiding(VkCommandBuffer command);
    void completeLightGuiding();
    std::vector<std::unique_ptr<Buffer>> uploads;
    // One mapped staging buffer per frame slot, filled front to back by
    // upload(). Reset once the slot's fence has completed (complete()).
    struct Staging {
      std::unique_ptr<Buffer> buffer;
      VkDeviceSize used = 0;
    } staging;
    VkDeviceSize stage(const void *data, size_t size);
    std::vector<std::shared_ptr<Accel>> retired;
    struct GeometryGroup {
      uint32_t surface = 0, count = 0;
      VkDeviceSize indices = 0, triangleMap = 0;
      bool opaque = false;
    };
    struct Geometry {
      std::unique_ptr<Buffer> input;
      std::vector<VkAccelerationStructureGeometryKHR> geometries;
      std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
      std::vector<uint32_t> counts;
      std::vector<VkDeviceSize> indexOffsets;
      std::vector<GeometryGroup> groups;
      std::shared_ptr<Accel> accel;
      uint64_t fingerprint = 0, revision = 0;
      uint64_t topologyHash = 0; // GeometryScratch::topologyHash of input
      float diagonal = 0;
      std::array<float, 3> buildLow{}, buildHigh{};
      uint32_t refits = 0;
      bool deform = false, pending = true, update = false;
      VkAccelerationStructureBuildSizesInfoKHR sizes = {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    };
    struct MeshView {
      std::weak_ptr<scene::Mesh> owner;
      std::shared_ptr<const scene::MeshUpdateStream> updateStream;
      std::shared_ptr<Geometry> geometry;
      std::vector<std::shared_ptr<Buffer>> vertices;
      std::vector<scene::Matrix> bones;
      std::vector<uint64_t> attributeHashes; // GeometryScratch::attributeHashes of vertices
      std::vector<uint64_t> normalHashes;    // GeometryScratch::normalHashes of vertices
      uint64_t fingerprint = 0, lastUse = 0;
      uint64_t contentDigest = 0; // REMIX_FRAME_DIGEST only: hash of the uploaded data.
    };
    // CPU half of a mesh view conversion: skinning, BLAS input assembly and
    // content hashes. Independent per view, so conversions run on workers.
    struct GeometryScratch {
      std::string content;
      std::vector<std::vector<TraceVertex>> shading;
      std::vector<VkDeviceSize> vertexOffsets, indexOffsets;
      std::vector<uint32_t> counts, vertexCounts;
      std::vector<GeometryGroup> groups;
      // Hashes combine per-part hashes; each input byte is hashed once.
      std::vector<uint64_t> shadingHashes;  // Per surface: positions, attributes and normals
      std::vector<uint64_t> positionHashes; // Per surface: scene::fastHash of BLAS positions
      // Per surface: texture coordinates and colors. With topologyHash (every
      // content byte except positions), they decide a delta upload.
      std::vector<uint64_t> attributeHashes;
      // Per surface: normals. Unchanged normals are not uploaded by a delta.
      std::vector<uint64_t> normalHashes;
      uint64_t contentHash = 0;            // topologyHash and positionHashes
      std::vector<float> attributeScratch, normalScratch; // Per-surface gathers, reused
      uint64_t topologyHash = 0;
      float low[3] = {}, high[3] = {};
      size_t bytes() const {
        size_t size = content.capacity() + shading.capacity() * sizeof(std::vector<TraceVertex>) +
                      (vertexOffsets.capacity() + indexOffsets.capacity()) * sizeof(VkDeviceSize) +
                      (counts.capacity() + vertexCounts.capacity()) * sizeof(uint32_t) +
                      groups.capacity() * sizeof(GeometryGroup) +
                      (shadingHashes.capacity() + positionHashes.capacity() + attributeHashes.capacity() +
                       normalHashes.capacity()) * sizeof(uint64_t) +
                      (attributeScratch.capacity() + normalScratch.capacity()) * sizeof(float);
        for (const auto &vertices : shading)
          size += vertices.capacity() * sizeof(TraceVertex);
        return size;
      }
    };
    // A view that needs new geometry this frame (meshView), between its CPU
    // conversion (convertMesh, any thread) and its GPU upload (commitMesh).
    struct MeshConversion {
      MeshView *view = nullptr;
      const scene::Draw *draw = nullptr;
      const scene::OpacityPartition *const *partitions = nullptr;
      std::string opaque;
      bool skinned = false, streaming = false, deform = false;
      std::shared_ptr<Geometry> previousGeometry;
      GeometryScratch data;
    };
    // Finds the draw's view. When it needs new geometry, prepares conversion
    // (view state updated as the serial path does) and returns true.
    MeshView &meshView(const scene::Draw &draw, const scene::OpacityPartition *const *partitions,
                       size_t partitionCount, MeshConversion &conversion, bool &convert);
    struct InstanceView {
      MeshView *view = nullptr;
      int32_t conversion = -1; // index into conversions, or -1
    };
    std::vector<InstanceView> instanceViews;
    std::vector<const scene::OpacityPartition *> partitionStorage;
    // Read-only: the view meshView would return unchanged for a draw that is
    // not streamed (no lineage transfer) and needs no conversion, else null.
    // Safe to run concurrently; marks the view used this frame.
    MeshView *findView(const scene::Draw &draw, const scene::OpacityPartition *const *partitions,
                       size_t partitionCount);
    std::vector<size_t> partitionOffsets;
    static void convertMesh(MeshConversion &conversion);
    void commitMesh(VkCommandBuffer command, MeshConversion &conversion);
    std::vector<MeshConversion> conversions; // reused storage; see Gpu::prepare
    std::unique_ptr<WorkerPool> workers;
    struct Tlas {
      std::shared_ptr<Accel> accel;
      std::unique_ptr<Buffer> instances;
      trace::TlasPolicy policy;
      // Combined TLAS only: the combinedStaticStamp whose static instances
      // this slot's instance buffer already holds (0: none).
      uint64_t prefix = 0;
    } tlas[3]; // static, dynamic, combined (rtx.native.pathtrace.combinedTlas)
    // Combined TLAS: its policy and the static part's instance records, kept
    // while the static inputs do not change.
    trace::CombinedTlasPolicy combinedPolicy;
    std::vector<VkAccelerationStructureInstanceKHR> combinedStatic, combinedDynamic;
    // The frame's sun and sky (PathTracer::overlayLighting).
    OverlayLighting overlay;
    uint64_t combinedStaticStamp = 0;
    bool combinedStaticValid = false;
    void buildCombinedTop(VkCommandBuffer command, const std::vector<trace::InstanceInput> &staticInputs,
                          const std::vector<trace::InstanceInput> &dynamicInputs, bool force,
                          uint64_t staticGeneration);
    struct ContentHash {
      size_t operator()(const std::string &content) const {
        return size_t(scene::fastHash(content.data(), content.size()));
      }
    };
    std::unordered_map<std::string, std::weak_ptr<Geometry>, ContentHash> geometryCache;
    // Persistent static set ("rtx.native.scene.persistentStatic"): the units
    // (with their temporal history) and the frame tables that keep their
    // surface table entries across frames. The Env of the tables: the
    // tracer's mesh views.
    struct TableViews {
      using View = MeshView;
      using VertexTable = scene::TemporalTable<std::shared_ptr<trace::detail::Buffer>>;
      using Span = scene::TraceFrame::InstanceList::ConstSpan;
      PathTracer::Gpu *gpu = nullptr;
      VkCommandBuffer command = VK_NULL_HANDLE;
      void resolveViews(const std::vector<Span> &instances, std::vector<const MeshView *> &views);
      bool findViews(const std::vector<Span> &instances, std::vector<const MeshView *> &views);
      std::shared_ptr<const void> owner(const MeshView &view, uint32_t surface) const {
        return view.vertices.at(surface);
      }
      [[noreturn]] void tooManyObjects() const { throw VK_ERROR_TOO_MANY_OBJECTS; }
    };
    scene::StaticScene statics;
    trace::FrameTables<TableViews> tables;
    std::vector<std::shared_ptr<const void>> retiredStates;
    uint64_t prepareSerial = 0, renderedSerial = 0;
    // The frame tables' prefix version this slot's surface table holds.
    uint64_t tableVersion = 0;
    // The static prefix's texture descriptors, resolved once per prefix.
    std::vector<VkDescriptorImageInfo> prefixTextures;
    std::vector<uint32_t> prefixSrgbSlots; // prefixTextures' sRGB-encoded slots
    uint64_t prefixTexturesGeneration = 0;
    bool staticValidation = false, staticMismatchLogged = false;
    PathTracer::StaticStatistics staticStatistics;
    scene::OpacityCache opacityCache;
    scene::SurfaceEmissionCache surfaceEmissionCache;
    using MeshViewKey = std::tuple<const scene::Mesh *, uint64_t, uint32_t, std::string>;
    struct MeshViewKeyHash {
      size_t operator()(const MeshViewKey &key) const {
        const auto &opaque = std::get<3>(key);
        const uint64_t values[] = {uint64_t(reinterpret_cast<uintptr_t>(std::get<0>(key))), std::get<1>(key),
                                   std::get<2>(key), scene::fastHash(opaque.data(), opaque.size())};
        return size_t(scene::fastHash(values, sizeof(values)));
      }
    };
    // Looked up once per traced instance per frame; iteration order is unused
    // beyond collecting BLAS builds, whose results do not depend on order.
    std::unordered_map<MeshViewKey, MeshView, MeshViewKeyHash> meshes;
    // Update lineage -> its current view. Keyed like MeshViewKey with the
    // stream in place of the mesh: skinned lineages keep one view per instance.
    using UpdateViewKey = std::tuple<const scene::MeshUpdateStream *, uint64_t, uint32_t, std::string>;
    std::map<UpdateViewKey, MeshViewKey> updateViews;
    // Views of per-instance skins (keys with an instance), which retire when
    // unused; rigid views retire only when their CPU mesh is released, so
    // retirement scans them only after scene::meshReleases changed.
    std::unordered_set<const MeshViewKey *> deformViews;
    uint64_t meshReleasesSeen = ~0ull;
    std::vector<std::shared_ptr<Geometry>> retiredGeometry;
    std::vector<std::shared_ptr<Buffer>> retiredBuffers;
    std::vector<std::shared_ptr<Geometry>> compactPending;
    remixapi_TracingStatisticsEXT stats = {.structSize = sizeof(remixapi_TracingStatisticsEXT)};
    struct Sharc {
      std::unique_ptr<Buffer> hash, accumulation, resolved, counters, readback;
      VkPipeline update{}, resolve{};
      VkQueryPool timestamps{};
      bool clear = true, pending = false, hasReceivers = false;
      scene::TemporalEpoch epoch{};
      uint64_t reset = 0, warmFrames = 0;
      uint32_t scale = 0, bounces = 0, responsiveFrames = 0, warmTarget = 64;
      // Frames left of the window dynamic geometry shortens, and this frame's
      // update grid (sharcDownscale, coarser above sharcMaxUpdatePaths).
      uint32_t motionFrames = 0, downscale = 1;
      uint32_t localConfig[4] = {};
      float localIntensity = 0, localRange = 0;
      float previousEye[3] = {}, light[9] = {};
      std::vector<uint8_t> geometry, dynamicGeometry;
      uint64_t geometryGeneration = 0; // the frame tables' static prefix geometry holds
      remixapi_SharcStatisticsEXT stats{.structSize = sizeof(remixapi_SharcStatisticsEXT)};
    } sharc;
    struct LinearCapture {
      std::unique_ptr<Buffer> readback;
      std::string path;
      VkExtent2D extent{};
      bool packedPbr = false, hitDistance = false;
    } linearCapture;
    void captureLinear(VkCommandBuffer command);
    void completeLinearCapture();
    struct FrameResources {
      std::string traceState;
      LinearCapture linearCapture;
      VkDescriptorSet set = VK_NULL_HANDLE;
      VkQueryPool timestamps = VK_NULL_HANDLE, compaction = VK_NULL_HANDLE, sharcTimestamps = VK_NULL_HANDLE;
      uint32_t compactionCapacity = 0;
      bool timingPending = false, sharcPending = false, guidingPending = false, reusePending = false,
           rayPending = false;
      std::unique_ptr<Buffer> table, uniform, scratch, topScratch, instances[3];
      uint64_t combinedPrefix = 0; // Tlas::prefix of the combined TLAS's instances
      std::unique_ptr<Buffer> tableUpload;
      std::vector<uint8_t> tableShadow;
      std::unique_ptr<Buffer> lightNodes, lightEmitters, torchNodes, torchEmitters, emissiveNodes,
          emissiveEmitters;
      std::unique_ptr<Buffer> sharcReadback, guidingReadback, reuseReadback, rayReadback;
      uint64_t lightGeneration = 0, torchGeneration = 0, emissiveGeneration = 0;
      uint64_t lightInstance = 0, torchInstance = 0, emissiveInstance = 0;
      std::vector<std::unique_ptr<Buffer>> uploads;
      Staging staging;
      PrimaryRecording primaryRecording;
      std::vector<std::shared_ptr<Accel>> retired;
      std::vector<std::shared_ptr<Geometry>> compactPending, retiredGeometry;
      std::vector<std::shared_ptr<Buffer>> retiredBuffers;
      scene::TemporalTable<std::shared_ptr<Buffer>> retiredVertices;
      std::vector<std::shared_ptr<const void>> retiredStates;
      uint64_t tableVersion = 0;
    } frames[gpuFrameCount];
    uint32_t frameIndex = 0;
    void swapFrame(FrameResources &frame);
    void selectFrame(uint32_t slot);
    void beginSharc(const TraceOptions &options);
    void destroySharc(bool temporalOnly = false);
    // geometryGeneration: nonzero when geometry is the frame tables' static
    // prefix of that generation (equal generations, equal bytes).
    void configureSharc(Frame &frame, const TraceOptions &options, scene::TemporalEpoch epoch,
                        const std::vector<uint8_t> &geometry, uint64_t geometryGeneration,
                        const std::vector<uint8_t> &dynamicGeometry, bool hasReceivers);
    void recordSharc(VkCommandBuffer command);
    void finishSharc(VkCommandBuffer command);
    // Delta uploads (commitMesh): shading vertices assembled on the GPU from new
    // positions and normals (0: the previous version's, unchanged) and the
    // previous version's texture coordinates and colors. Jobs run after this frame's copies, before the BLAS builds.
    struct AssembleJob {
      VkDeviceAddress positions = 0, normals = 0, previous = 0, target = 0;
      uint32_t count = 0, reserved = 0;
    };
    static_assert(sizeof(AssembleJob) == 40);
    VkPipelineLayout assembleLayout = VK_NULL_HANDLE;
    VkPipeline assemble = VK_NULL_HANDLE;
    bool assembleFailed = false; // Creation failed: always upload in full.
    std::vector<AssembleJob> assembleJobs;
    void createAssemblePipeline();
    void recordAssemble(VkCommandBuffer command);
    // A lighting pipeline compiled with guidance off (its variants are lazy).
    struct LightingRequest {
      trace::Stage stage;
      VkPipelineLayout layout = VK_NULL_HANDLE;
      VkPipeline *out = nullptr;
      bool supportsGuiding = true, supportsRoulette = true;
      bool splitDirect = false;
      bool variants = true; // selected through selectLightingPipeline: prewarm its variants
      bool separateIndirect = false; // an opaque pass built as the separate indirect pass's consumer
      bool background = false; // compiled off the awaited batch; requested again when needed
    };
    // Decompresses and compiles the requests whose pipeline does not exist yet
    // as one batch, concurrently on the compiler workers, and registers each as a lighting
    // pipeline. Throws a failure after every request has completed. While
    // deferLighting is set (configuration preparation) it only schedules the
    // batch; finishLightingPipelines() then awaits every scheduled batch, so
    // separate passes' pipelines compile together. No pass reads its pipeline
    // handles before recording.
    void createLightingPipelines(const std::vector<LightingRequest> &requests);
    // createLightingPipelines that waits (up to two minutes) for pipelines
    // another worker is still compiling, for passes a frame cannot go without.
    void awaitLightingPipelines(const std::vector<LightingRequest> &requests);
    void finishLightingPipelines();
    bool deferLighting = false;
    std::vector<LightingRequest> deferredLighting;
    trace::SceneSignature lastScene;
    uint64_t sequence = 0;
    float previousDirectClamp = 0, previousIndirectClamp = 0;
    bool previousFireflyFilter = false;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    bool preparing = false, preparedRasterPrimary = true;
    bool preparedPerspective = false, preparedLocal = false;
    void createRawPipelines();
    void configureFrame(const TraceOptions &options, bool perspective,
                        bool local);
    ~Gpu();
    std::shared_ptr<trace::detail::BufferCache> bufferCache = std::make_shared<trace::detail::BufferCache>();
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags);
    std::unique_ptr<Buffer> buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host = false,
                                   bool reusable = false);
    void write(Buffer &buffer, const void *data, size_t size);
    // Writes consecutive parts; offset skips bytes the buffer already holds.
    void write(Buffer &buffer, std::initializer_list<std::pair<const void *, size_t>> parts, size_t offset = 0);
    void hostBuffer(std::unique_ptr<Buffer> &out, const void *data, size_t size, VkBufferUsageFlags usage);
    std::unique_ptr<Buffer> upload(VkCommandBuffer command, const void *data, size_t size,
                                   VkBufferUsageFlags usage);
    std::shared_ptr<Accel> acceleration(VkDeviceSize size, VkAccelerationStructureTypeKHR type);
    VkBuildAccelerationStructureFlagsKHR flags(bool deform) const;
    MeshView &mesh(VkCommandBuffer command, const scene::Draw &draw,
                   const std::vector<const scene::OpacityPartition *> &partitions);
    uint64_t geometryTicks = 0; // FramePerformanceEXT geometry time, per-instance timer.
    void complete(VkCommandBuffer command);
    void buildBottom(VkCommandBuffer command);
    std::unique_ptr<Buffer> topScratch;
    // staticGeneration: nonzero when staticInputs are the frame tables' static
    // prefix of that generation; one the static TLAS was built from (or found
    // equal to) is not compared again.
    // faceCulling: rtx.native.pathtrace.hardwareFaceCulling (trace::instanceFlags).
    // combined: rtx.native.pathtrace.combinedTlas; builds only tlas[2].
    void buildTop(VkCommandBuffer command, const std::vector<trace::InstanceInput> &staticInputs,
                  const std::vector<trace::InstanceInput> &dynamicInputs, bool force, uint64_t staticGeneration,
                  bool faceCulling, bool combined);
    uint64_t tlasStaticGeneration = 0;
    bool topFaceCulling = false; // the setting the TLAS instance flags were built with
    bool topCombined = false;    // the TLAS layout last built
    std::unique_ptr<Image> image(bool colorAttachment = false,
                                 VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT, VkExtent2D size = {});
    void destroyPost();
    void beginFrame(const TraceOptions &options);
    void destroyTarget();
    void prepare(VkCommandBuffer command, RasterRenderer &resources, const TraceOptions &options,
                 scene::TraceFrame &scene);
  };
} // namespace remix
