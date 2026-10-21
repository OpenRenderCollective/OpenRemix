#include "forward/bake/job.h"
#include "forward/bake/probe_visibility_plan.h"
#include "forward/bake/probe_visibility_budget.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <stdexcept>

namespace remix::forward::bake {
  void Controller::Job::prepareProbeVisibility(uint64_t storageRange, float cutoff, bool groupedDirect) {
    const auto &policy = mixedLights->lights.directLighting;
    const bool directional = groupedDirect || std::any_of(policy.lights.begin(), policy.lights.end(), [](DirectMode mode) {
      return aggregatesDiffuse(mode) && retainsVisibility(mode);
    });
    if (lookupBytes(grid) >= storageRange) return;
    const auto storage = probeVisibilityBudget(uint32_t(grid.probes.size()), uint32_t(source->lights.size()),
                                               storageRange - lookupBytes(grid), directional);
    if (!storage.pairs) return;
    auto plan = planProbeVisibility(grid, source->lights, cutoff, storage.pairs, cancellation.get_token(), policy,
                                    groupedDirect);
    if (plan && !plan->tasks.empty()) {
      localProbeTasks = std::move(plan->tasks);
      localProbes.emplace(uint32_t(grid.probes.size()), uint32_t(source->lights.size()),
                          localProbeTasks, storage.bytes, directional);
      std::vector<bool> indexed(source->lights.size());
      for (const auto &[identity, light] : mixedLights->lights.identities) indexed[light] = true;
      for (const auto &task : localProbeTasks)
        if (!indexed[task.light]) {
          mixedLights->lights.identities.emplace(source->lights[task.light].id, task.light);
          indexed[task.light] = true;
        }
    }
    std::fprintf(stderr,
                 "Forward local probe visibility: %zu pairs, %u selected / %u eligible lights, "
                 "%u deferred, %llu candidate tests, cap %u, directional bytes %llu, budget bytes %llu\n",
                 localProbeTasks.size(), plan ? plan->selectedLights : 0,
                 plan ? plan->eligibleLights : 0, plan ? plan->deferredLights : 0,
                 static_cast<unsigned long long>(plan ? plan->candidates : 0), storage.pairs,
                 static_cast<unsigned long long>(localProbes ? localProbes->direct().size_bytes() : 0),
                 static_cast<unsigned long long>(storage.bytes));
  }
  void Controller::recordProbeLocal(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (!job.localProbesPlanned) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(m_physical, &properties);
      const auto range = properties.limits.maxStorageBufferRange;
      const auto owner = m_job;
      const auto cutoff = m_lightCutoff;
      static std::atomic<uint64_t> generation = 0;
      job.localProbesPlanned = true;
      job.ticket = m_pipelines->prepare("forward-probe-volumes-" + std::to_string(++generation),
          [owner, range, cutoff, grouped = config.groupedStyledDirect] { owner->prepareProbeVisibility(range, cutoff, grouped); });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Planning local probe light volumes");
      return;
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
