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
      const auto &policy = job.mixedLights->lights.directLighting.lights;
      const bool directional = std::any_of(policy.begin(), policy.end(), [](DirectMode mode) {
        return aggregatesDiffuse(mode) && retainsVisibility(mode);
      });
      const uint64_t base = sizeof(GridHeader) + job.grid.probes.size() * sizeof(Result);
      const uint64_t offsets = (job.grid.probes.size() + 1) * sizeof(uint32_t);
      const uint64_t range = properties.limits.maxStorageBufferRange;
      const uint64_t wireOverhead = 32 + ((job.source->lights.size() + 31) / 32) * sizeof(uint32_t) + 15 +
                                    ProbeVisibility::maximumDirectoryBytes;
      // Bound trace work and storage. Each selected light has its complete
      // volume; deferred lights keep ordinary shadows and unknown probe masks.
      if (!job.source->lights.empty() && range > base + offsets + wireOverhead) {
        const uint64_t budget = std::min(ProbeVisibility::maximumBytes, range - base - wireOverhead);
        const uint32_t pairs =
            uint32_t(std::min<uint64_t>(131072, (budget - offsets) / (sizeof(ProbeVisibility::Entry) +
                (directional ? sizeof(ProbeVisibility::Direct) : 0))));
        auto plan = planProbeVisibility(job.grid, job.source->lights, m_lightCutoff, pairs,
                                        job.cancellation.get_token(), job.mixedLights->lights.directLighting);
        if (plan && !plan->tasks.empty()) {
          job.localProbeTasks = std::move(plan->tasks);
          job.localProbes.emplace(uint32_t(job.grid.probes.size()), uint32_t(job.source->lights.size()),
                                  job.localProbeTasks, budget, directional);
          std::vector<bool> indexed(job.source->lights.size());
          for (const auto &[identity, light] : job.mixedLights->lights.identities)
            indexed[light] = true;
          for (const auto &task : job.localProbeTasks)
            if (!indexed[task.light]) {
              job.mixedLights->lights.identities.emplace(job.source->lights[task.light].id, task.light);
              indexed[task.light] = true;
            }
        }
        std::fprintf(stderr,
                     "Forward local probe visibility: %zu pairs, %u selected / %u eligible lights, "
                     "%u deferred, %llu candidate tests, cap %u, directional bytes %llu\n",
                     job.localProbeTasks.size(), plan ? plan->selectedLights : 0,
                     plan ? plan->eligibleLights : 0, plan ? plan->deferredLights : 0,
                     static_cast<unsigned long long>(plan ? plan->candidates : 0), pairs,
                     static_cast<unsigned long long>(job.localProbes ? job.localProbes->direct().size_bytes() : 0));
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
                                           config.samples, job.localProbes->directional());
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
