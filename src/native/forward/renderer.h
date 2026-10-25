#pragma once
#include "forward/bake/content.h"
#include "forward/options.h"
#include "forward/probes/data.h"
#include "forward/ray/reflections.h"
#include "forward/shadows/plan.h"
#include "raster/renderer.h"
#include "postfx/renderer.h"
#include <memory>
#include <remix/remix_bake_ext.h>
namespace remix::pipeline {
  class Service;
}

namespace remix::forward {
  namespace probes { class Authoring; }
  namespace bake {
    class Controller;
  }
  struct Statistics {
    struct CpuRecording {
      struct Preparation {
        double selection = 0, draws = 0, lighting = 0;
        double uploads = 0, materials = 0, publication = 0;
      } preparation;
      double bake = 0, liveScene = 0, frame = 0, world = 0;
      double reflections = 0, waterExposure = 0, publication = 0;
    } cpuRecording;
    uint32_t samples = 1, lights = 0, draws = 0, clusters = 0;
    uint32_t shadowedLights = 0, shadowOverflow = 0, staticShadowUpdates = 0;
    uint32_t staticSunUpdates = 0, capturedCasters = 0, liveCasters = 0;
    uint32_t staticCasterPreparations = 0, emptyMovingShadowMaps = 0;
    struct LocalShadowDraws {
      uint64_t lightId = 0;
      bool active = false;
      std::array<uint32_t, shadows::faces> frozen{}, moving{};
    };
    // Submitted draw calls per cubemap face, not distinct objects or GPU samples.
    std::array<LocalShadowDraws, shadows::maximumLights> localShadowDraws{};
    uint32_t clusterLightCapacity = clusterCapacity;
    uint32_t clusteredLights = 0, fullyBakedLights = 0;
    uint32_t clusterBuildLights = 0; // Broad-phase candidates, including any fallback tail.
    uint32_t analyticLights = 0, aggregateLights = 0, selectedAggregateLights = 0;
    // Positive selected-aggregate response weights; the roles can overlap.
    uint32_t selectedHighlightLights = 0, selectedMixedShadowLights = 0;
    uint32_t requiredMixedLights = 0; // Clustered subset whose host requests a mixed response.
    struct ProbeVisibility {
      uint32_t lights = 0, groups = 0;
      uint64_t fieldBytes = 0, residentBytes = 0, peakResidentBytes = 0;
      uint64_t selectionHash = 0, recordedGathers = 0, completedReuses = 0;
    } probeVisibility;
    uint64_t targetBytes = 0, geometryBytes = 0, bufferBytes = 0;
    uint64_t rigidUploadCreates = 0, rigidUploadReuses = 0, spareGeometryBytes = 0;
    uint64_t accelerationBytes = 0;
    bake::ContentHashes bakeContent;
    ray::Statistics rayReflections;
    ray::Statistics rayGI;
    postfx::Statistics postProcessing;
    probes::Header reflectionProbes;
    probes::LayerIdentities reflectionIdentities{};
    uint32_t boundReflectionDraws = 0, unresolvedReflectionDraws = 0;
    probes::AuthoringStatistics authoredReflections;
    struct IrradianceGrid {
      std::array<uint32_t, 3> dimensions{};
      uint32_t slots = 0, usable = 0;
      float spacingMetres = 0;
      float finestSpacingMetres = 0;
      uint32_t coarseRecords = 0;
      std::array<uint32_t, 3> patchCounts{}; // 2x, 4x, 8x; not uniform coverage
    } irradianceGrid;
    float reflectionSceneScale = 1;
    remixapi_BakeStatisticsEXT baking{.structSize = sizeof(remixapi_BakeStatisticsEXT)};
  };
  class Renderer {
  public:
    Renderer();
    ~Renderer();
    // Preparation may run on a worker; activation and retirement require an
    // idle queue. RasterRenderer owns textures, and frames retain draw owners
    // until their fences complete.
    void initialize(VkPhysicalDevice physical, VkDevice device, VkRenderPass output, VkExtent2D extent,
                    Options options, bool encodeOutput, pipeline::Service *pipelines = nullptr,
                    bool hardwareRay = false, std::shared_ptr<bake::Controller> baking = {},
                    uint32_t queueFamily = VK_QUEUE_FAMILY_IGNORED,
                    std::shared_ptr<postfx::State> exposure = {}, bool hardwareSrgb = false,
                    std::shared_ptr<probes::Authoring> authoredReflections = {});
    // Target replacements may share the map bake. Only the active renderer
    // advances it, on the render thread after the owning frame-slot fence.
    std::shared_ptr<bake::Controller> bakeController() const;
    std::shared_ptr<postfx::State> exposureState() const;
    // Shared across compatible target replacements. Preparation only retains
    // ownership; the active render thread advances captures after slot fences.
    std::shared_ptr<probes::Authoring> reflectionAuthoring() const;
    // Queue-idle activation retains published bake metadata before the first frame.
    void activate(uint32_t slot);
    void selectFrame(uint32_t slot); // caller has waited for this slot's fence
    // Negative animationSeconds uses the renderer clock. Explicit time supports
    // deterministic rendering tests independently of exposure adaptation.
    void prepare(VkCommandBuffer command, RasterRenderer &resources, float seconds = -1,
                 double animationSeconds = -1);
    void composite(VkCommandBuffer command);  // inside the output render pass
    void background(VkCommandBuffer command); // before raster sky and the world composite
    const Statistics &statistics() const;
    const Options &options() const;
    bool owns(const scene::Draw &draw) const;
    void updateLighting(const Options &options); // target-compatible edits, on the render thread

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::forward
