#pragma once
#include "forward/bake/content.h"
#include "forward/bake/grid.h"
#include "forward/lightmap/geometry.h"
#include "forward/mixed/data.h"
#include "forward/mixed/map.h"
#include "forward/probes/data.h"
#include "forward/resources.h"
#include "platform/gpu_frames.h"
#include "runtime/pipelines.h"
#include <remix/remix_bake_ext.h>

namespace remix {
  class RasterRenderer;
}
namespace remix::forward::bake {
  struct Volume {
    uint64_t hash = 0;
    ContentHashes content;
    bool directDiffuse = false;
    scene::DistantLight sun; // shape provenance; isDistant only when surface sunlight was baked
    probes::Header reflectionHeader;
    float sceneScale = 1;
    std::array<uint32_t, 4> occlusionParameters{}; // words per chart, chart count, texture width, reserved
    std::unique_ptr<Buffer> data, specular, staging;
    std::unique_ptr<Image> surfaceAtlas;
    std::unique_ptr<mixed::OcclusionMap> occlusion;
    std::shared_ptr<lightmap::Geometry> surfaces;
    std::shared_ptr<const mixed::Bindings> mixedLights;
  };
  struct Memory {
    uint64_t buffers = 0, images = 0, acceleration = 0;
  };
  // Render-thread controller. selectFrame observes only the slot whose fence
  // the caller completed. Submitted frames retain their job and volume owners.
  class Controller {
  public:
    Controller(VkPhysicalDevice physical, VkDevice device, pipeline::Service *pipelines, bool hardware,
               uint32_t queueFamily = VK_QUEUE_FAMILY_IGNORED);
    ~Controller();
    // Constructor-only identity; replacement-target workers may validate it
    // without inspecting mutable jobs, frame submissions or published data.
    bool compatible(VkPhysicalDevice physical, VkDevice device, pipeline::Service *pipelines, bool hardware,
                    uint32_t queueFamily = VK_QUEUE_FAMILY_IGNORED) const {
      return physical == m_physical && device == m_device && pipelines == m_pipelines &&
             bool(m_statistics.supported) == bool(hardware && pipelines) && queueFamily == m_queueFamily;
    }
    void selectFrame(uint32_t slot);
    void record(VkCommandBuffer command, RasterRenderer &resources, const Config &config,
                float lightCutoff = .001f);
    const std::shared_ptr<Volume> &volume() const { return m_volume; }
    VkDescriptorBufferInfo descriptor() const;
    VkDescriptorBufferInfo reflectionDescriptor() const;
    VkDescriptorImageInfo surfaceDescriptor() const;
    VkDescriptorImageInfo occlusionDescriptor() const;
    const remixapi_BakeStatisticsEXT &statistics() const { return m_statistics; }
    Memory memory() const;

  private:
    struct Job;
    struct Submission {
      std::shared_ptr<Job> job;
      std::shared_ptr<Volume> upload;
      std::shared_ptr<Volume> sampled;
      uint32_t first = 0, count = 0;
      bool preparation = false, radiance = false, surface = false, visibility = false, reflectionSite = false,
           sun = false, overflowVisibility = false;
    };
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    pipeline::Service *m_pipelines;
    uint32_t m_slot = 0;
    uint32_t m_queueFamily;
    Config m_config;
    uint64_t m_hash = 0;
    float m_lightCutoff = .001f;
    std::shared_ptr<Job> m_job;
    std::shared_ptr<const scene::BakeScene> m_source;
    std::vector<std::shared_ptr<Job>> m_obsolete;
    std::shared_ptr<Volume> m_volume;
    std::array<Submission, gpuFrameCount> m_submissions;
    std::unique_ptr<Buffer> m_empty;
    std::unique_ptr<Image> m_emptySurface;
    std::unique_ptr<mixed::OcclusionMap> m_emptyOcclusion;
    bool m_emptySurfaceReady = false;
    remixapi_BakeStatisticsEXT m_statistics{};
    void invalidate();
    void status(uint32_t state, const char *message);
    void activate(VkCommandBuffer command);
    void recordSurfaces(VkCommandBuffer command, const Config &config);
    void recordVisibility(VkCommandBuffer command, const Config &config);
    void recordOverflowVisibility(VkCommandBuffer command, const Config &config);
    void recordSun(VkCommandBuffer command, const Config &config);
    void recordReflections(VkCommandBuffer command, const Config &config);
  };
} // namespace remix::forward::bake
