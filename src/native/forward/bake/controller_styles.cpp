#include "forward/bake/job.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    std::atomic<uint64_t> styleGeneration{0};
  }
  void Controller::recordStyles(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    auto &work = *job.styles;
    if (work.received()) {
      const auto owner = m_job;
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(m_physical, &properties);
      const auto storageRange = properties.limits.maxStorageBufferRange;
      job.finishStage.store(Job::FinishStage::Styles, std::memory_order_relaxed);
      job.ticket = m_pipelines->prepare(
          "forward-style-" + std::to_string(++styleGeneration),
          [owner, config, storageRange] {
            owner->styles->encode(owner->surfaceLayout, config.denoiseLightmaps,
                                  owner->cancellation.get_token());
            if (owner->styles->complete()) {
              owner->styleWords = owner->styles->finish(owner->cancellation.get_token());
              owner->styles.reset();
              owner->prepareGroupedStyles(storageRange, config);
              owner->filtering = true;
            }
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Encoding styled indirect lightmaps");
      return;
    }
    const auto &tasks = work.probeTasks();
    const bool surface = work.probeNext == tasks.size();
    auto &next = surface ? work.surfaceNext : work.probeNext;
    const auto total = surface ? work.surfaceCount() : uint32_t(tasks.size());
    if (next < total) {
      auto &submission = m_submissions[m_slot];
      if (submission.job) throw std::logic_error("Styled bake slot reused before its fence");
      const auto samples = surface ? config.samples : config.probeSampleCount();
      const auto count = job.count(surface ? Work::Surface : Work::Irradiance,
                                   config, total - next, samples, config.bounces);
      std::vector<Point> probes;
      std::span<const Point> points;
      if (surface) {
        points = std::span(job.surfaceLayout.points).subspan(next, count);
      } else {
        probes.reserve(count);
        for (uint32_t i = 0; i < count; ++i) probes.push_back(job.grid.points[tasks[next + i]]);
        points = probes;
      }
      job.transport->recordComponent(command, m_slot, points, work.style(), samples, config.bounces);
      submission.job = m_job;
      submission.surface = surface;
      submission.styled = work.style();
      submission.first = next;
      submission.count = count;
      next += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Styled indirect %u (%u/%u): probes %u/%zu, surfaces %u/%u",
                  work.style(), work.ordinal(), work.count(), work.probeReceived, tasks.size(),
                  work.surfaceReceived, work.surfaceCount());
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
} // namespace remix::forward::bake
