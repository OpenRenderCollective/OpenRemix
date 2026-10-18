#include "forward/bake/job.h"
#include "forward/bake/cache.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    std::atomic<uint64_t> styleGeneration{0};
  }
  void Controller::Job::prepareIndexedVisibility() {
    const char *override = std::getenv("REMIX_DEBUG_UNIFIED_MASKS");
    if ((override && std::strcmp(override, "0") == 0) || surfacePixels.empty()) return;
    const auto began = std::chrono::steady_clock::now();
    const uint64_t visibilityBytes = uint64_t(surfaceLayout.pageSize) * surfaceLayout.pageSize * surfaceLayout.pages * 8;
    const uint64_t budget = std::min<uint64_t>(mixed::Masks::maximumPublishedBytes,
        mixed::Masks::maximumBytes + visibilityBytes);
    unifiedMasks = mixed::unifyMasks(*mixedLights, surfaceLayout, surfacePixels, budget, cancellation.get_token());
    std::fprintf(stderr, "Forward unified visibility preparation: %.3f ms, fits %u\n",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count(),
        unsigned(bool(unifiedMasks)));
  }
  std::unique_ptr<const cache::Snapshot> Controller::Job::takeCacheSnapshot() {
    auto snapshot = std::make_unique<cache::Snapshot>();
    snapshot->grid.header = grid.header;
    snapshot->grid.probes = std::move(grid.probes);
    snapshot->grid.tasks = std::move(grid.tasks);
    // Keep only serialized allocations/mappings, never transient sample arrays.
    snapshot->layout.pageSize = surfaceLayout.pageSize;
    snapshot->layout.pages = surfaceLayout.pages;
    snapshot->layout.luxelMetres = surfaceLayout.luxelMetres;
    snapshot->layout.seamSamples = surfaceLayout.seamSamples;
    snapshot->layout.curvedSeamSamples = surfaceLayout.curvedSeamSamples;
    snapshot->layout.charts = std::move(surfaceLayout.charts);
    snapshot->layout.mappings = std::move(surfaceLayout.mappings);
    snapshot->mixed.lights = std::move(mixedLights->lights);
    snapshot->mixed.charts = std::move(mixedLights->charts);
    snapshot->mixed.directCharts = std::move(mixedLights->directCharts);
    snapshot->mixed.overflow = mixedLights->overflow;
    snapshot->mixed.occlusion = std::move(mixedLights->occlusion);
    snapshot->mixed.masks = std::move(mixedLights->masks);
    snapshot->captures = std::move(captures);
    snapshot->specular = std::move(specular);
    snapshot->pixels = std::move(surfacePixels);
    snapshot->styles = std::move(styleWords);
    snapshot->localProbes = std::move(localProbes);
    snapshot->surfaceSamples = surfaceReceived;
    snapshot->directDiffuse = hasDirectDiffuse;
    snapshot->sun = hasSun;
    return snapshot;
  }
  void Controller::recordStyles(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    auto &work = *job.styles;
    if (work.received()) {
      const auto owner = m_job;
      job.finishStage.store(Job::FinishStage::Styles, std::memory_order_relaxed);
      job.ticket = m_pipelines->prepare(
          "forward-style-" + std::to_string(++styleGeneration),
          [owner, config] {
            owner->styles->encode(owner->surfaceLayout, config.denoiseLightmaps,
                                  owner->cancellation.get_token());
            if (owner->styles->complete()) {
              owner->styleWords = owner->styles->finish(owner->cancellation.get_token());
              owner->styles.reset();
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
      const auto count = job.count(surface ? Work::Surface : Work::Irradiance,
                                   config, total - next, config.samples, config.bounces);
      std::vector<Point> probes;
      std::span<const Point> points;
      if (surface) {
        points = std::span(job.surfaceLayout.points).subspan(next, count);
      } else {
        probes.reserve(count);
        for (uint32_t i = 0; i < count; ++i) probes.push_back(job.grid.points[tasks[next + i]]);
        points = probes;
      }
      job.transport->recordComponent(command, m_slot, points, work.style(), config.samples, config.bounces);
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
