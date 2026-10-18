#pragma once
#include "forward/bake/content.h"
#include "forward/options.h"
#include "forward/probes/data.h"
#include "forward/ray/reflections.h"
#include "raster/renderer.h"
#include "postfx/renderer.h"
#include <memory>
#include <remix/remix_bake_ext.h>
namespace remix::pipeline {
  class Service;
}

namespace remix::forward {
  namespace bake {
    class Controller;
  }
  struct Statistics {
    struct CpuRecording {
      double bake = 0, liveScene = 0, frame = 0, world = 0;
      double reflections = 0, waterExposure = 0, publication = 0;
    } cpuRecording;
    uint32_t samples = 1, lights = 0, draws = 0, clusters = 0;
    uint32_t shadowedLights = 0, shadowOverflow = 0, staticShadowUpdates = 0;
    uint32_t staticSunUpdates = 0, capturedCasters = 0, liveCasters = 0;
    uint32_t staticCasterPreparations = 0;
    uint32_t clusterLightCapacity = clusterCapacity;
    uint64_t targetBytes = 0, geometryBytes = 0, bufferBytes = 0;
    uint64_t accelerationBytes = 0;
    bake::ContentHashes bakeContent;
    ray::Statistics rayReflections;
    ray::Statistics rayGI;
    postfx::Statistics postProcessing;
    probes::Header reflectionProbes;
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
                    std::shared_ptr<postfx::State> exposure = {}, bool hardwareSrgb = false);
    // Target replacements may share the map bake. Only the active renderer
    // advances it, on the render thread after the owning frame-slot fence.
    std::shared_ptr<bake::Controller> bakeController() const;
    std::shared_ptr<postfx::State> exposureState() const;
    void selectFrame(uint32_t slot); // caller has waited for this slot's fence
    void prepare(VkCommandBuffer command, RasterRenderer &resources, float seconds = -1);
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
