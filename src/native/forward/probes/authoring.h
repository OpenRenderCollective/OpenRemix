#pragma once
#include "forward/probes/data.h"
#include "forward/probes/image_asset.h"
#include "forward/probes/coverage_baseline.h"
#include "scene/reflection_volumes.h"
#include "forward/resources.h"
#include "runtime/pipelines.h"
#include "platform/gpu_frames.h"
#include <memory>
namespace remix { class RasterRenderer; }
namespace remix::forward::bake { struct Volume; }
namespace remix::forward::probes {
  // Reflection-only publication. Neither source ownership nor diffuse bake
  // buffers are modified; frame slots retain every sampled and submitted job.
  struct ReflectionSnapshot {
    std::shared_ptr<const ImageAsset> image;
    CoverageBaselineSet baseline;
    LayerIdentities identities{};
  };
  struct AuthoringMemory { uint64_t buffers = 0, images = 0, acceleration = 0; };
  class Authoring {
  public:
    Authoring(VkPhysicalDevice physical, VkDevice device, pipeline::Service *pipelines,
               bool hardware, uint32_t queueFamily);
    ~Authoring();
    // Immutable constructor identity: safe to inspect during worker preparation.
    bool compatible(VkPhysicalDevice physical, VkDevice device, pipeline::Service *pipelines,
                    bool hardware, uint32_t queueFamily) const {
      return physical == m_physical && device == m_device && pipelines == m_pipelines &&
             hardware == m_hardware && queueFamily == m_queueFamily;
    }
    void selectFrame(uint32_t slot);
    void record(VkCommandBuffer command, RasterRenderer &resources, const bake::Config &config,
                const std::shared_ptr<const bake::Volume> &automatic, bool fallback);
    ReflectionSnapshot snapshot(const std::shared_ptr<const bake::Volume> &automatic) const;
    Descriptors descriptor(Descriptors automatic) const;
    const Header *header() const;
    const LayerIdentities *identities() const;
    uint32_t receiverLayer(uint64_t sceneId, uint64_t volumeId) const; // one-based; zero uses ordinary selection
    AuthoringMemory memory() const;
    AuthoringStatistics statistics() const;
    const std::string &status() const { return m_status; }
  private:
    struct Job;
    struct Frame {
      std::shared_ptr<Job> work, sampled, uploaded;
      uint32_t first = 0, count = 0;
      bool preparation = false;
      float budgetMilliseconds = 0;
    };
    VkPhysicalDevice m_physical;
    VkDevice m_device;
    pipeline::Service *m_pipelines;
    bool m_hardware;
    uint32_t m_queueFamily, m_slot = 0;
    std::shared_ptr<const scene::BakeScene> m_source;
    std::shared_ptr<const scene::ReflectionVolumes> m_request;
    std::weak_ptr<const bake::Volume> m_automatic;
    uint64_t m_revision = 0;
    bool m_fallback = false;
    std::shared_ptr<Job> m_job, m_active;
    std::vector<std::shared_ptr<Job>> m_retired;
    std::array<Frame, gpuFrameCount> m_frames;
    bake::Config m_config;
    std::string m_status;
    void retire();
  };
} // namespace remix::forward::probes
