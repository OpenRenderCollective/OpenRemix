#pragma once
#include "forward/probes/data.h"
#include "forward/probes/image_asset.h"
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
  struct AuthoringMemory { uint64_t buffers = 0, images = 0, acceleration = 0; };
  class Authoring {
  public:
    Authoring(VkPhysicalDevice physical, VkDevice device, pipeline::Service *pipelines,
               bool hardware, uint32_t queueFamily);
    ~Authoring();
    void selectFrame(uint32_t slot);
    void record(VkCommandBuffer command, RasterRenderer &resources, const bake::Config &config,
                const std::shared_ptr<const bake::Volume> &automatic, bool fallback);
    Descriptors descriptor(Descriptors automatic) const;
    const Header *header() const;
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
