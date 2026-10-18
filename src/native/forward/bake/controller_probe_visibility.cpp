#include "forward/bake/job.h"
#include "forward/bake/probe_visibility_plan.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace remix::forward::bake {
  void Controller::recordProbeLocal(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (!job.localProbesPlanned) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(m_physical, &properties);
      const uint64_t base = sizeof(GridHeader) + job.grid.probes.size() * sizeof(Result);
      const uint64_t offsets = (job.grid.probes.size() + 1) * sizeof(uint32_t);
      const uint64_t range = properties.limits.maxStorageBufferRange;
      // Bound new trace work as well as storage. A declined complete plan keeps
      // ordinary shadows; never publish a truncated set as complete coverage.
      if (!job.source->lights.empty() && range > base + offsets + 32) {
        const uint64_t budget = std::min(ProbeVisibility::maximumBytes, range - base - 32);
        const uint32_t pairs =
            uint32_t(std::min<uint64_t>(131072, (budget - offsets) / sizeof(ProbeVisibility::Entry)));
        auto plan = planProbeVisibility(job.grid, job.source->lights, m_lightCutoff, pairs,
                                        job.cancellation.get_token());
        if (plan && !plan->tasks.empty()) {
          job.localProbeTasks = std::move(plan->tasks);
          job.localProbes.emplace(uint32_t(job.grid.probes.size()), uint32_t(job.source->lights.size()),
                                  job.localProbeTasks, budget);
          std::vector<bool> indexed(job.source->lights.size());
          for (const auto &[identity, light] : job.mixedLights->lights.identities)
            indexed[light] = true;
          for (const auto &task : job.localProbeTasks)
            if (!indexed[task.light]) {
              job.mixedLights->lights.identities.emplace(job.source->lights[task.light].id, task.light);
              indexed[task.light] = true;
            }
        }
        std::fprintf(stderr, "Forward local probe visibility: %zu pairs, plan %s, cap %u\n",
                     job.localProbeTasks.size(), plan ? "accepted" : "budget fallback", pairs);
      }
      job.localProbesPlanned = true;
    }
    if (job.localProbeReceived == job.localProbeTasks.size()) {
      recordSurfaces(command, config);
      return;
    }
    if (job.localProbeNext < job.localProbeTasks.size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Local probe visibility slot reused before its fence");
      const uint32_t count =
          job.count(Work::Visibility, config, uint32_t(job.localProbeTasks.size() - job.localProbeNext),
                    config.samples);
      job.transport->recordVisibilityTasks(command, m_slot, job.grid.points,
                                           std::span(job.localProbeTasks).subspan(job.localProbeNext, count),
                                           config.samples);
      submission.job = m_job;
      submission.probeLocal = true;
      submission.first = job.localProbeNext;
      submission.count = count;
      job.localProbeNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Baking local probe visibility (%u/%zu pairs)",
                  job.localProbeReceived, job.localProbeTasks.size());
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
} // namespace remix::forward::bake
