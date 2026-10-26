#pragma once
// Objective-C++ only: the Metal path tracer's GPU state (pathtrace/gpu.h's
// counterpart). The frame tables, opacity proofs, light hierarchies, static
// set and temporal history are the shared CPU implementations.
#include "metal/denoise.h"
#include <cstdlib>
#include <cstring>
#include "metal/post.h"
#include "metal/reconstruction.h"
#include "metal/raster_gpu.h"
#include "metal/tracer.h"
#include "metal/frame.h"
#include "pathtrace/frame_tables.h"
#include "pathtrace/tlas_policy.h"
#include "platform/timing.h"
#include "platform/workers.h"
#include "scene/emission.h"
#include "scene/light_bvh.h"
#include "scene/opacity.h"
#include "scene/static_scene.h"
#include <map>
#include <tuple>
#include <unordered_set>

namespace remix::metal::trace {
  using remix::trace::TraceSurface;
  struct TraceVertex {
    float position[4] = {};
    float uv[4] = {};
    float color[4] = {};
  };
  static_assert(sizeof(TraceVertex) == 48);
  // The tracing shaders' uniform block (pathtrace/gpu.h Frame).
  // The shaders' per-frame uniform: metal/frame.h.
  // gbuffer.vert/frag push constants (pathtrace/gpu.h PrimaryPush).
  struct PrimaryPush {
    scene::Matrix model;
    float jitter[2] = {};
    uint32_t surfaceIndex = 0, stableDepth = 0;
    std::array<float, 4> reverseDepth{};
  };
  static_assert(sizeof(PrimaryPush) == 96);

  // A shared buffer and its GPU address (buffer_reference).
  struct Buffer {
    id<MTLBuffer> handle = nil;
    uint64_t address = 0;
    size_t size = 0;
  };
  // A BLAS. address is an identity for the frame tables and TLAS inputs
  // (Vulkan's device address): Metal instances name their BLAS by index.
  struct Accel {
    id<MTLAccelerationStructure> handle = nil;
    uint64_t address = 0;
  };
  struct GeometryGroup {
    uint32_t surface = 0, count = 0;
    size_t indices = 0, triangleMap = 0;
    bool opaque = false;
  };
  struct Geometry {
    std::shared_ptr<Buffer> input;
    std::vector<uint32_t> counts, vertexCounts;
    std::vector<size_t> vertexOffsets, indexOffsets;
    std::vector<GeometryGroup> groups;
    std::shared_ptr<Accel> accel;
    uint64_t fingerprint = 0, revision = 0;
    float diagonal = 0;
    bool deform = false, pending = true;
  };
  struct MeshView {
    std::weak_ptr<scene::Mesh> owner;
    std::shared_ptr<const scene::MeshUpdateStream> updateStream;
    std::shared_ptr<Geometry> geometry;
    std::vector<std::shared_ptr<Buffer>> vertices;
    std::vector<scene::Matrix> bones;
    uint64_t fingerprint = 0, lastUse = 0, contentDigest = 0;
  };
  // CPU half of a mesh view conversion (pathtrace/gpu.h GeometryScratch).
  struct GeometryScratch {
    std::string content;
    std::vector<std::vector<TraceVertex>> shading;
    std::vector<size_t> vertexOffsets, indexOffsets;
    std::vector<uint32_t> counts, vertexCounts;
    std::vector<GeometryGroup> groups;
    std::vector<uint64_t> shadingHashes;
    uint64_t contentHash = 0;
    float low[3] = {}, high[3] = {};
  };
  struct MeshConversion {
    MeshView *view = nullptr;
    const scene::Draw *draw = nullptr;
    const scene::OpacityPartition *const *partitions = nullptr;
    std::string opaque;
    bool skinned = false, streaming = false, deform = false;
    std::shared_ptr<Geometry> previousGeometry;
    GeometryScratch data;
  };
} // namespace remix::metal::trace

namespace remix::metal {
  struct PathTracer::Gpu {
    using Buffer = trace::Buffer;
    using Accel = trace::Accel;
    using Geometry = trace::Geometry;
    using MeshView = trace::MeshView;
    using MeshConversion = trace::MeshConversion;
    using Frame = trace::Frame;

    id<MTLDevice> device = nil;
    Configuration configuration;
    id<MTLRenderPipelineState> primaryPipeline = nil, stableDepthPipeline = nil, composite[2] = {};
    ArgumentSet stableVertexSet, stableFragmentSet;
    std::unique_ptr<reconstruction::Renderer> reconstruction;
    id<MTLDepthStencilState> primaryDepth = nil, compositeDepth[2] = {};
    ArgumentSet primaryVertexSet, primaryFragmentSet, compositeSet, postSet;
    // A pass of the tracing stages (pathtrace/gpu.h LightingPipeline and
    // LightingRequest): its stage, descriptor sets 0 (tracing) and 1 (temporal
    // inputs, when the stage has them), what its variants may specialize, and
    // its base pipeline's function constants.
    struct Lighting {
      shaders::Stage stage{};
      ArgumentSet set0, set1;
      bool supportsGuiding = false, supportsRoulette = true;
      bool variants = true;             // selected by the scene's variant and features (else base, defaults)
      bool baseSeparateIndirect = false; // the base consumes the separate indirect pass
      uint32_t base = 0;                // the base pipeline's constants
      explicit operator bool() const { return bool(set0); }
    };
    struct LightingFlags {
      bool supportsGuiding = true, supportsRoulette = true, variants = true, separateIndirect = false;
      bool background = false; // the base compiles in the background (selected as null until ready)
    };
    // The configuration's passes (pathtrace/renderer.cpp createRawPipelines,
    // temporal.cpp beginFrame): every lighting stage queries SHARC itself
    // (lightingStage), gated per frame by frame.sharcConfig.
    Lighting opaque, indirect, transparency, singleTransparency, sheet, frontGuides, psr, confidence;
    Lighting sharcUpdate, sharcResolve, regirPresample, volumeUpdate, sparseCompact, sparseArgs;
    // ORCA (orca.glsl) is not on Metal: its cache sums radiance with 64-bit
    // atomic adds, which Metal (and SPIRV-Cross's MSL) does not have.
    Lighting *lightingPasses[14] = {&opaque,        &indirect,     &transparency,   &singleTransparency,
                                    &sheet,         &frontGuides,  &psr,            &confidence,
                                    &sharcUpdate,   &sharcResolve, &regirPresample, &volumeUpdate,
                                    &sparseCompact, &sparseArgs};
    // Sparse rendering (pathtrace/sparse.cpp): this frame's selected pixels,
    // listed by a compaction pass for the indirect launch of the opaque pass.
    struct Sparse {
      std::shared_ptr<Buffer> list, count;
      bool active = false, launched = false;
      float rate = 1, launchedRate = 1;
      const char *inactiveReason = "not requested";
    } sparse;
    void configureSparse(id<MTLCommandBuffer> command, Frame &frame, const TraceOptions &options);
    void recordSparse(id<MTLCommandBuffer> command);
    // Volumetrics (pathtrace/volume.cpp, volume.glsl): the medium's scattered
    // light in nested world-space grids, one buffer for every level's cells.
    struct Volume {
      std::shared_ptr<Buffer> cells;
      bool active = false, valid = false;
      uint32_t cellsPerSide = 0, levels = 0, counter = 0;
      float cellUnits = 0, eye[3] = {};
      size_t bytes = 0;
      const char *inactiveReason = "not requested";
    } volume;
    void configureVolume(id<MTLCommandBuffer> command, Frame &frame, const TraceOptions &options);
    void recordVolume(id<MTLCommandBuffer> command);
    // The scene's lighting selection this frame (local_lights.cpp
    // selectLightingPipeline's inputs).
    struct LightingState {
      bool local = false, guided = false, smallSet = false, resampling = false, angular = false;
      bool firstHit = false, roulette = false, decals = true, earlyOut = false, profile = false;
    } lightingState;
    // A selected pipeline and its per-dispatch switches (pathtrace.comp
    // `features`, pushed at offset 28).
    struct Selected {
      id<MTLComputePipelineState> pipeline = nil;
      uint32_t features = 1; // trace::detail::defaultFeatures: decals
    };
    // pathtrace/local_lights.cpp selectLightingPipeline: the pass's variant
    // for this frame's state, compiled in the background (the base meanwhile).
    // A role without a fallback (a sparse launch, an indirect-pass consumer
    // whose base is not one) selects nil until its variant has compiled.
    Selected select(const Lighting &pass, bool separateIndirect = false, uint32_t sparse = 0);
    static shaders::Stage lightingStage(shaders::Stage uncached);
    // The separate indirect pass (pathtrace/temporal.cpp configureIndirect):
    // bounces traced in their own dispatch, consumed by the opaque pass.
    // REMIX_SPLIT_INDIRECT=0 turns it off.
    struct IndirectPass {
      std::shared_ptr<Buffer> results; // 16 bytes a pixel
      bool active = false;
      uint32_t mode = [] {
        const char *value = std::getenv("REMIX_SPLIT_INDIRECT");
        return value && !std::strcmp(value, "0") ? 0u : 2u;
      }();
    } indirectPass;
    void configureIndirect(Frame &frame, const TraceOptions &options);
    std::shared_ptr<Buffer> sheetResults; // the sheet lighting pass's transport (16 bytes a pixel)
    // Presampled light pools per world cell (pathtrace/regir.cpp, regir.glsl):
    // this frame's registrations, last frame's cells, and the slot pool.
    struct Regir {
      static constexpr uint32_t capacity = 1u << 15, slots = 64;
      std::shared_ptr<Buffer> cells[2], pool;
      uint32_t writeIndex = 0;
      bool valid = false, active = false;
    } regir;
    void configureRegir(Frame &frame, const TraceOptions &options, bool localActive);
    // Local light guiding (pathtrace/light_guiding.cpp): learned visibility per
    // world cell, two banks (read last frame's, train this frame's).
    struct LightGuiding {
      static constexpr uint32_t capacity = 65536;
      std::shared_ptr<Buffer> history[2];
      uint32_t writeIndex = 0;
      bool valid = false, active = false;
      uint64_t generation = 0;
      float range = 0, cellSize = 2;
      bool resampled = false;
      remixapi_LocalLightGuidingStatisticsEXT stats{.structSize = sizeof(remixapi_LocalLightGuidingStatisticsEXT)};
    } lightGuiding;
    void configureLightGuiding(Frame &frame, const TraceOptions &options, bool resampling);
    void beginLightGuiding(id<MTLCommandBuffer> command);
    void finishLightGuiding(id<MTLCommandBuffer> command);
    void readLightGuiding();
    // Sky guiding (pathtrace/sky_guiding.cpp): per world cell and facing, a
    // histogram of the directions its first-surface sky rays found the sky in,
    // trained this frame and read the next (two banks, like lightGuiding).
    struct SkyGuiding {
      static constexpr uint32_t capacity = 16384, stride = 68; // words per slot: key, total, 2 reserved, 64 bins
      std::shared_ptr<Buffer> history[2];
      uint32_t writeIndex = 0;
      bool valid = false, active = false;
      float cellSize = 0;
    } skyGuiding;
    void configureSkyGuiding(Frame &frame, const TraceOptions &options);
    void beginSkyGuiding(id<MTLCommandBuffer> command);
    // pathtrace/ray_profile.cpp (REMIX_RAY_PROFILE=file): structural ray
    // counters from one pixel in 64, appended to a CSV per completed frame.
    // Timing comparisons must run without it.
    struct RayProfile {
      const char *path = std::getenv("REMIX_RAY_PROFILE");
      std::shared_ptr<Buffer> counters; // 3 passes x 64 words
    } rayProfile;
    void beginRayProfile(id<MTLCommandBuffer> command, Frame &frame);
    void finishRayProfile(id<MTLCommandBuffer> command);
    void readRayProfile();
    void recordRegir(id<MTLCommandBuffer> command);
    void createLighting(Lighting &pass, shaders::Stage stage, LightingFlags flags);
    void bindTracing(Lighting &pass, const std::vector<id<MTLTexture>> &textures,
                     const std::vector<id<MTLSamplerState>> &samplers);
    // indirect: the threadgroup counts at that offset of the buffer
    // (vkCmdDispatchIndirect), otherwise width x height threads.
    void dispatch(id<MTLCommandBuffer> command, const Lighting &pass, const Selected &selected, const char *label,
                  uint32_t width, uint32_t height, id<MTLBuffer> indirect = nil, NSUInteger offset = 0);
    // Temporal denoising (pathtrace/temporal.cpp beginFrame's post* flags are
    // this tracer's configuration).
    std::unique_ptr<denoise::Renderer> denoiser;
    denoise::Frame temporalFrame{};
    uint64_t previousLocalGeneration = 0;
    float previousLocalIntensity = 0, previousLocalRange = 0;
    uint32_t previousLocalActive = 0, previousLocalAdaptive = 0, localResponsiveFrames = 0;
    bool previousLocalBlueNoise = false;
    float previousSun[9] = {}, pendingSun[9] = {};
    scene::Matrix previousView{}, previousProjection{}, pendingProjection{};
    uint32_t previousSeed = 0;
    std::chrono::steady_clock::time_point previousTime{}, pendingTime{};
    // SHARC (pathtrace/sharc.cpp).
    struct Sharc {
      std::shared_ptr<Buffer> hash, accumulation, resolved, counters;
      id<MTLBuffer> readback[gpuFrameCount] = {};
      bool pending[gpuFrameCount] = {};
      bool clear = true, hasReceivers = false;
      scene::TemporalEpoch epoch{};
      uint64_t reset = 0, warmFrames = 0;
      uint32_t scale = 0, bounces = 0, responsiveFrames = 0, motionFrames = 0, downscale = 1;
      uint32_t localConfig[4] = {};
      float localIntensity = 0, localRange = 0;
      float previousEye[3] = {}, light[9] = {};
      std::vector<uint8_t> geometry, dynamicGeometry;
      uint64_t geometryGeneration = 0;
      remixapi_SharcStatisticsEXT stats{.structSize = sizeof(remixapi_SharcStatisticsEXT)};
    } sharc;
    void beginSharc();
    void readSharc();
    void configureSharc(Frame &frame, const TraceOptions &options, scene::TemporalEpoch epoch,
                        const std::vector<uint8_t> &geometry, uint64_t geometryGeneration,
                        const std::vector<uint8_t> &dynamicGeometry, bool hasReceivers);
    void recordSharc(id<MTLCommandBuffer> command);
    void finishSharc(id<MTLCommandBuffer> command);
    id<MTLTexture> world = nil, transmission = nil, sky = nil, primary[7] = {};
    // Automatic exposure and bloom; the composite reads their results.
    std::unique_ptr<post::Renderer> post;
    uint32_t width = 0, height = 0;
    bool falseColorView = false, rendered = false;
    uint32_t debugView = 0;

    // Mesh views (pathtrace/geometry.cpp).
    using MeshViewKey = std::tuple<const scene::Mesh *, uint64_t, uint32_t, std::string>;
    struct MeshViewKeyHash {
      size_t operator()(const MeshViewKey &key) const {
        const auto &opaque = std::get<3>(key);
        const uint64_t values[] = {uint64_t(reinterpret_cast<uintptr_t>(std::get<0>(key))), std::get<1>(key),
                                   std::get<2>(key), scene::fastHash(opaque.data(), opaque.size())};
        return size_t(scene::fastHash(values, sizeof(values)));
      }
    };
    std::unordered_map<MeshViewKey, MeshView, MeshViewKeyHash> meshes;
    using UpdateViewKey = std::tuple<const scene::MeshUpdateStream *, uint64_t, uint32_t, std::string>;
    std::map<UpdateViewKey, MeshViewKey> updateViews;
    std::unordered_set<const MeshViewKey *> deformViews;
    struct ContentHash {
      size_t operator()(const std::string &content) const {
        return size_t(scene::fastHash(content.data(), content.size()));
      }
    };
    std::unordered_map<std::string, std::weak_ptr<Geometry>, ContentHash> geometryCache;
    uint64_t meshReleasesSeen = ~0ull, nextAccel = 1;
    std::vector<MeshConversion> conversions;
    struct InstanceView {
      MeshView *view = nullptr;
      int32_t conversion = -1;
    };
    std::vector<InstanceView> instanceViews;
    std::vector<const scene::OpacityPartition *> partitionStorage;
    std::vector<size_t> partitionOffsets;
    MeshView &meshView(const scene::Draw &draw, const scene::OpacityPartition *const *partitions,
                       size_t partitionCount, MeshConversion &conversion, bool &convert);
    MeshView *findView(const scene::Draw &draw, const scene::OpacityPartition *const *partitions,
                       size_t partitionCount);
    static void convertMesh(MeshConversion &conversion);
    void commitMesh(MeshConversion &conversion);
    std::vector<std::shared_ptr<Geometry>> pendingBuilds;

    // The frame tables' Env (pathtrace/gpu.h TableViews).
    struct TableViews {
      using View = MeshView;
      using VertexTable = scene::TemporalTable<std::shared_ptr<Buffer>>;
      using Span = scene::TraceFrame::InstanceList::ConstSpan;
      PathTracer::Gpu *gpu = nullptr;
      void resolveViews(const std::vector<Span> &instances, std::vector<const MeshView *> &views);
      bool findViews(const std::vector<Span> &instances, std::vector<const MeshView *> &views);
      std::shared_ptr<const void> owner(const MeshView &view, uint32_t surface) const {
        return view.vertices.at(surface);
      }
      [[noreturn]] void tooManyObjects() const { throw Error("too many traced objects"); }
    };
    scene::StaticScene statics;
    remix::trace::FrameTables<TableViews> tables;
    scene::TemporalHistory history;
    using VertexHistory = scene::TemporalTable<std::shared_ptr<Buffer>>;
    VertexHistory previousVertices, pendingVertices, retiredPreviousVertices;
    std::vector<std::shared_ptr<const void>> retiredStates;
    uint64_t prepareSerial = 0;
    scene::OpacityCache opacityCache;
    scene::SurfaceEmissionCache surfaceEmissionCache;
    std::unique_ptr<WorkerPool> workers;

    // Acceleration structures.
    struct Tlas {
      id<MTLAccelerationStructure> handle = nil;
      id<MTLBuffer> instances = nil;
      std::vector<id<MTLAccelerationStructure>> blas;
      remix::trace::TlasPolicy policy;
    } tlas[2]; // static, dynamic
    std::unordered_map<uint64_t, std::weak_ptr<Accel>> accels; // address -> BLAS
    id<MTLBuffer> scratch = nil;
    id<MTLAccelerationStructure> emptyTlas = nil;
    void buildBottom(id<MTLCommandBuffer> command);
    void buildTop(id<MTLCommandBuffer> command, const std::vector<remix::trace::InstanceInput> &staticInputs,
                  const std::vector<remix::trace::InstanceInput> &dynamicInputs, bool force, bool faceCulling);
    id<MTLAccelerationStructure> build(id<MTLAccelerationStructureCommandEncoder> encoder,
                                       MTLAccelerationStructureDescriptor *descriptor);

    // Lighting and per-frame buffers.
    scene::LightBvh lightBvh, torchBvh, emissiveBvh;
    scene::EmissionHistory emissionHistory;
    std::shared_ptr<Buffer> lightNodes, lightEmitters, blueNoise;
    uint64_t lightBufferGeneration = 0;
    remix::trace::SceneSignature lastScene;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();
    remixapi_TracingStatisticsEXT stats{.structSize = sizeof(remixapi_TracingStatisticsEXT)};
    // The last completed frame's GPU times, written by completion handlers.
    struct GpuTimes {
      std::atomic<float> build{0}, trace{0};
      std::atomic<bool> available{false};
    };
    std::shared_ptr<GpuTimes> gpuTimes = std::make_shared<GpuTimes>();
    remixapi_FramePerformanceEXT performance{.structSize = sizeof(remixapi_FramePerformanceEXT)};
    uint64_t geometryTicks = 0, materialTicks = 0, temporalTicks = 0;
    scene::Matrix previousCamera{}, pendingCamera{};
    float previousJitter[2] = {}, jitter[2] = {};
    std::array<float, 4> primaryReverseDepth{};

    struct FrameResources {
      std::shared_ptr<Buffer> table, uniform;
      std::shared_ptr<Buffer> movedLights; // scene::MovedLight pairs read by this frame
      id<MTLBuffer> guideCounters = nil;   // light guiding's training counters, copied at the frame's end
      bool guidePending = false;
      id<MTLBuffer> rayCounts = nil; // the ray profile's counters, copied at the frame's end
      bool rayPending = false;
      std::vector<id> retired;
      std::vector<std::shared_ptr<Geometry>> retiredGeometry;
      std::vector<std::shared_ptr<Buffer>> retiredBuffers;
      std::vector<std::shared_ptr<const void>> retiredStates;
    } frames[gpuFrameCount];
    uint32_t frameIndex = 0;
    FrameResources &frame() { return frames[frameIndex]; }
    // Buffers the shaders reach by address (buffer_reference): every live one
    // is made resident for the passes that may read it.
    std::shared_ptr<Buffer> buffer(const void *data, size_t size);
    std::vector<std::weak_ptr<Buffer>> addressable;
    std::vector<id<MTLResource>> resident;
    void collectResident();
    void retire(id object) {
      if (object)
        frame().retired.push_back(object);
    }
    void createTarget();
    void createPipelines();
    void createComposite(MTLPixelFormat format);
    void submittedFrame();
    MTLPixelFormat displayFormat = MTLPixelFormatInvalid;
    void prepare(id<MTLCommandBuffer> command, Raster &resources, const TraceOptions &options,
                 scene::TraceFrame &prepared);
    void recordPrimaryPass(id<MTLCommandBuffer> command, bool stable);
  };
} // namespace remix::metal
