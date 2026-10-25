#pragma once
#include "denoise/renderer.h"
#include "pathtrace/options.h"
#include "runtime/settings.h"
#include "runtime/sky_control.h"
#include "postfx/renderer.h"
#include "raster/renderer.h"
#include "scene/trace_frame.h"
#include <remix/remix_native_ext.h>
#include "pathtrace/block_costs.h"
#include <vulkan/vulkan.h>

namespace remix::trace {
  // The surface table step of the slowest frame since this was last read
  // (diagnostics): its size, and what growing its buffers, writing it and
  // finding the changed blocks took.
  struct TableCosts {
    double total = 0, grow = 0, write = 0, compare = 0;
    uint64_t bytes = 0, capacity = 0, grown = 0, copied = 0;
  };
  inline TableCosts g_tableCosts;

  // New blocks of buffer memory since this was last read (diagnostics): how
  // each was come by, and the slowest. `ready`: a spare allocated ahead was
  // there; `awaited`: one was still being allocated; `none`: no spare of that
  // memory type (its first block); `large`: a buffer over the block size.
  struct BlockCosts {
    uint32_t ready = 0, awaited = 0, none = 0, large = 0;
    double slowest = 0;
    uint64_t slowestBuffer = 0;
    uint32_t slowestType = 0, slowestHow = 0, slowestBlocks = 0;
    bool slowestHost = false;
    // The slowest binding of a buffer to its memory and mapping of a block.
    double slowestBind = 0, slowestMap = 0, slowestCreate = 0;
    bool bindFirst = false; // that binding was the first into its block
  };
  inline BlockCosts g_blockCosts;

  // The acceleration structure step of the slowest frame since this was last
  // read (diagnostics), by what it called. `now` is the frame being recorded.
  struct AccelCosts {
    double total = 0, bottom = 0, top = 0, create = 0, record = 0, queryPool = 0, instances = 0, sizes = 0;
    // The combined top level: its policy's decision, the instance records,
    // their write, the structure and scratch buffers, the build command.
    double decide = 0, fill = 0, write = 0, storage = 0, topRecord = 0, storageInstances = 0, storageAccel = 0;
    uint64_t written = 0;
    uint32_t created = 0, bottomBuilds = 0, topInstances = 0, staticFilled = 0;
    bool update = false, staticChanged = false, keptPrefix = false;
  };
  inline AccelCosts g_accelNow, g_accelCosts;

  // The CPU scene preparation (PathTracer::prepareScene) since this was last
  // read (diagnostics), summed and of its slowest frame, by step: the frame's
  // instances and emitters, the opacity cache prune, staging the lighting
  // state, gathering the lights, the per-family trees, the combined tree's
  // update and the light visibility table.
  struct SceneCosts {
    double total = 0, frame = 0, prune = 0, stage = 0, gather = 0, split = 0, tree = 0, visibility = 0;
  };
  struct SceneCostTotals {
    SceneCosts sum, slowest;
    uint64_t frames = 0;
    uint32_t lights = 0; // the combined tree's input in the slowest frame
  };
  inline SceneCostTotals g_sceneCosts;

  // Light hierarchy uploads since this was last read (diagnostics): whole
  // copies, copies of only the changes, bytes and copy regions.
  struct LightUploads {
    uint64_t whole = 0, partial = 0, bytes = 0, regions = 0;
  };
  inline LightUploads g_lightUploads;
} // namespace remix::trace

namespace remix {
  class PathTracer {
  public:
    static constexpr uint32_t textureLimit = 4096;
    PathTracer();
    ~PathTracer();
    void initialize(VkPhysicalDevice physical, VkDevice device,
                    uint32_t queueFamily, bool preparing = false,
                    bool rasterPrimary = true, std::shared_ptr<postfx::State> exposureState = {});
    std::shared_ptr<postfx::State> exposureState() const;
    VkExtent2D renderExtent() const;
    void setPostProcessing(postfx::Renderer *renderer, bool retainExposure = false);
    void configureTarget(VkRenderPass pass, VkExtent2D extent, bool encodeOutput, VkFormat depthFormat,
                         VkImageView depthView, bool hardwareSrgb = false);
    VkRenderPass skyPass() const;
    // The view depth of the frame being traced (the denoiser's guide, R32
    // float, GENERAL layout, render resolution): what host passes sample as
    // REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT. None without a denoiser.
    VkImageView viewDepth() const;
    // What lit the frame last prepared, for effects drawn over it (particles):
    // the sun (propagation direction, radiance), the sky (radiance; w above
    // zero with an environment, which `environment` samples through
    // domeTransform, scaled by the radiance), the clouds' shadow on the sun
    // (sky::Renderer::cloudShadow; a null view without one), the up axis in
    // effect and the scene's TLAS.
    struct OverlayLighting {
      float sunDirection[3] = {0, 0, -1};
      float sunRadiance[3] = {};
      float skyRadiance[4] = {};
      VkDescriptorImageInfo environment{};
      float domeTransform[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
      VkDescriptorImageInfo cloudShadow{};
      float cloudShadowTransform[3][4] = {};
      uint32_t cloudShadowSlices = 0;
      uint32_t upAxis = 2;
      VkAccelerationStructureKHR scene = VK_NULL_HANDLE;
    };
    OverlayLighting overlayLighting() const;
    upscale::Statistics upscalingStatistics() const;
    // What frame generation needs of the frame just prepared: the upscaler's
    // depth and motion and the cameras of this and the previous frame
    // (unjittered). Not valid without an upscaler.
    struct FrameGenerationInputs {
      upscale::FrameGenerationInputs images;
      scene::Matrix view{}, projection{}, previousView{}, previousProjection{};
      float frameMilliseconds = 0; // since the previous frame, as the upscaler was given
      bool reset = true; // no usable previous frame
      bool valid = false;
    };
    FrameGenerationInputs frameGeneration() const;
    void setNgx(std::shared_ptr<upscale::Ngx> ngx);
    void prepareConfiguration(const TraceOptions &options, bool perspective,
                              bool local, uint32_t failedBackends = 0,
                              const std::string &failure = {});
    bool finishPreparation(VkQueue queue);
    // Submits the setup of reconstruction recreated in place (a Ray
    // Reconstruction model or preset change in beginFrame) ahead of the frame.
    void finishReconstruction(VkQueue queue);
    void beginFrame(const TraceOptions &options, const RasterRenderer &resources);
    void selectFrame(uint32_t slot);
    void submitted();
    remixapi_ShadowStatisticsEXT shadowStatistics() const;
    remixapi_LocalLightStatisticsEXT localLightStatistics() const;
    remixapi_LocalLightGuidingStatisticsEXT localLightGuidingStatistics() const;
    remixapi_LocalLightSetStatisticsEXT localLightSetStatistics() const;
    remixapi_CameraCullingStatisticsEXT cameraCullingStatistics() const;
    remixapi_DenoisingStatisticsEXT denoisingStatistics() const;
    bool translucencyDenoiserActive() const;
    remixapi_SharcStatisticsEXT sharcStatistics() const;
    void destroyTarget();
    // Traces the resources' persistent static set (staticDrawn) from the
    // tracer's own copy of it; may expand the set into the draw list when a
    // frame cannot (NativeResources::expandStatic).
    scene::TraceFrame prepareScene(RasterRenderer &resources, const TraceOptions &options) const;
    // Gives a prepared frame's lighting state back to the tracer when the
    // frame is not traced (prepare takes it otherwise); a no-op after prepare.
    void returnScene(scene::TraceFrame &frame) const;
    void prepare(VkCommandBuffer command, RasterRenderer &resources, const TraceOptions &options,
                 scene::TraceFrame &scene);
    void record(VkCommandBuffer command);
    remixapi_TracingStatisticsEXT statistics() const;
    bool adaptiveTerminationAvailable() const;
    void resetPerformance();
    remixapi_FramePerformanceEXT performance() const;
    // Phases of prepare/record for FrameBreakdownEXT; reset with resetPerformance.
    remixapi_FrameBreakdownEXT breakdown() const;
    // REMIX_FRAME_DIGEST diagnostics: an address-free hash of the last
    // prepared frame's GPU inputs (surface table, instances, primary draws).
    uint64_t frameDigest() const;
    // The persistent static set in the last prepared frame (Diagnostics).
    struct StaticStatistics {
      bool active = false;                      // traced from the kept static set
      uint64_t units = 0, surfaces = 0;         // its traced instances and table entries
      uint64_t reusedSurfaces = 0;              // static entries taken from kept tables
      uint64_t rebuiltSurfaces = 0;             // static entries of units built this frame
      uint64_t dynamicSurfaces = 0;             // entries of the frame's other instances
      uint64_t rebuiltUnits = 0, changes = 0, temporalSurfaces = 0, relayouts = 0;
      double syncMilliseconds = 0;              // within scene preparation
      double tableMilliseconds = 0;             // within trace surfaces
      uint64_t validations = 0, mismatches = 0; // "persistentStaticValidation"
      char mismatch[128] = {};                  // the first mismatch found
    };
    StaticStatistics staticStatistics() const;
    // Rebuilds the kept static tables from scratch every frame and compares.
    void setStaticValidation(bool enabled);
    // The NUMOS sky: the profile's sky settings, the game's control over
    // them (remix_sky_ext.h), and why the device cannot run it (empty when
    // it can). Applies from the next prepared frame.
    void setSky(const config::SkySettings &settings, const config::SkyControl &control,
                const std::string &unavailable);
    struct SkyStatus {
      bool supported = false, active = false;
      std::string reason; // why the sky is not active, or empty
    };
    SkyStatus skyStatus() const;
    // What the sky did in the last recorded frame; any thread.
    config::SkyState skyState() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix
