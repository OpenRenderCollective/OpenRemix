#pragma once
#include "forward/probes/capture_step.h"
#include "forward/probes/capture_config.h"
#include "forward/probes/capture_source.h"
#include "platform/gpu_frames.h"
#include "runtime/pipelines.h"
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace remix::forward::bake { class TransportMemory; }

namespace remix {
class RasterRenderer;
}
namespace remix::scene {
struct BakeScene;
}
namespace remix::forward::probes {
// Reflection-only lifecycle. No lightmap/probe-volume ownership or readiness.
class StandaloneCapture {
public:
  struct Memory {
    uint64_t buffers = 0, images = 0, acceleration = 0;
  };
  StandaloneCapture(VkPhysicalDevice physical, VkDevice device,
                    pipeline::Service *pipelines, bool hardware,
                    uint32_t queueFamily);
  ~StandaloneCapture();
  bool compatible(VkPhysicalDevice physical, VkDevice device,
                  pipeline::Service *pipelines, bool hardware,
                  uint32_t family) const {
    return physical == m_physical && device == m_device &&
           pipelines == m_pipelines && hardware == m_hardware &&
           family == m_family;
  }
  bool wasUsed() const { return m_used.load(std::memory_order_relaxed); }
  void selectFrame(uint32_t slot); // owning frame fence completed
  void record(VkCommandBuffer command, RasterRenderer &resources,
              CaptureConfig config, uint32_t count, float detailTolerance);
  const CaptureSource &source() const { return m_active; }
  Memory memory(bake::TransportMemory *transportMemory = nullptr) const;
  float sceneScale() const;
  bool cacheHit() const { return m_cacheHit; }
  const std::string &status() const { return m_status; }

private:
  struct Job;
  struct Frame {
    std::shared_ptr<Job> job;
    std::shared_ptr<const ImageAsset> sampled;
    std::shared_ptr<ImageAsset> upload;
    uint32_t first = 0, count = 0;
    CaptureStep step = CaptureStep::None;
    bool preparation = false;
  };
  VkPhysicalDevice m_physical;
  VkDevice m_device;
  pipeline::Service *m_pipelines;
  bool m_hardware;
  std::atomic_bool m_used = false;
  uint32_t m_family, m_slot = 0, m_count = 0;
  float m_tolerance = 0;
  CaptureConfig m_config;
  std::shared_ptr<const scene::BakeScene> m_source;
  std::shared_ptr<Job> m_job;
  std::vector<std::shared_ptr<Job>> m_retired;
  std::array<Frame, gpuFrameCount> m_frames;
  CaptureSource m_active;
  bool m_cacheHit = false;
  std::string m_status;
  void retire();
};
} // namespace remix::forward::probes
