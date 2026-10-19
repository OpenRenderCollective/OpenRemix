#include "forward/bake/job.h"
#include "forward/bake/storage_options.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <new>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    using Clock = std::chrono::steady_clock;
    struct CpuTime {
      double &total;
      Clock::time_point began = Clock::now();
      bool active = true;
      void finish() {
        if (active) total += std::chrono::duration<double, std::milli>(Clock::now() - began).count();
        active = false;
      }
      ~CpuTime() { finish(); }
    };
  }
  void Controller::Job::logDirectionalTiming() const {
    const auto &timing = directionalTiming;
    std::fprintf(stderr, "Forward directional capture timing: wall_ms=%.3f worker_ms=%.3f submission_ms=%.3f "
                         "readback_ms=%.3f gpu_ms=%.3f batches=%llu budget_bytes=%llu timestamps=%u\n",
        std::chrono::duration<double, std::milli>(Clock::now() - timing.began).count(),
        timing.workerMs, timing.submissionMs, timing.readbackMs, timing.gpuMs,
        static_cast<unsigned long long>(timing.batches), static_cast<unsigned long long>(directionalBudget),
        unsigned(transport->timingSupported()));
  }
  void Controller::Job::advanceDirectionals(bool finish) {
    CpuTime worker{directionalTiming.workerMs};
    const auto cancelled = cancellation.get_token();
    const auto decline = [&] {
      directionalCapture.reset();
      directionalWords.clear();
      completeDirectionals = false;
      worker.finish();
      logDirectionalTiming();
    };
    try {
      if (!directionalAttempted) {
        directionalAttempted = true;
        directionalTiming.began = Clock::now();
        directionalBudget = mixed::directionalStorageBudget(surfaceLayout);
        const auto links = mixed::positiveDirectionalLinks(*mixedLights, cancelled);
        directionalFields = uint32_t(links.size());
        directionalCapture = std::make_unique<mixed::DirectionalCapture>(surfaceLayout,
            uint32_t(source->lights.size()), links, 256ull << 20,
            directionalBudget, cancelled);
      }
      for (;;) {
        if (finish) {
          const auto fields = uint32_t(directionalCapture->window().size());
          if (!directionalCapture->finish(cancelled)) {
            std::fprintf(stderr, "Forward complete directional capture declined: encoded storage budget\n");
            decline();
            return;
          }
          directionalField += fields;
        }
        if (directionalCapture->done()) {
          directionalWords = directionalCapture->words(cancelled);
          directionalCapture.reset();
          completeDirectionals = true;
          std::fprintf(stderr, "Forward complete directional capture: %u fields, %u reused, %llu traced points, %zu encoded bytes\n",
                       directionalFields, directionalReusedFields,
                       static_cast<unsigned long long>(directionalTracedPoints),
                       directionalWords.size() * sizeof(uint32_t));
          worker.finish();
          logDirectionalTiming();
          return;
        }
        // Bound latency and staging independently of the map's total overlap.
        const auto samples = directionalCapture->beginWindow(
            Budget::maximumPoints, mixed::DirectionalCapture::maximumWindowFields, cancelled);
        directionalPending.clear();
        directionalPending.reserve(samples);
        directionalNext = directionalReceived = 0;
        finish = true;
        for (const auto &field : directionalCapture->window()) {
          if (!field.count) continue;
          const auto &owners = mixedLights->directCharts[field.owner.chart];
          const auto owner = std::find(owners.begin(), owners.end(), field.owner.light);
          if (owner == owners.end() || direct.empty()) {
            for (uint32_t i = 0; i < field.count; ++i) directionalPending.push_back(field.first + i);
            continue;
          }
          // Reuse retained unit samples, including known-black responses.
          const auto channel = size_t(owner - owners.begin());
          ++directionalReusedFields;
          constexpr uint32_t batchSize = 4096;
          for (uint32_t offset = 0; offset < field.count;) {
            const auto first = field.first + offset;
            const auto tasks = directionalCapture->batch(first, std::min(batchSize, field.count - offset));
            std::vector<VisibilityResult> values(tasks.size());
            for (size_t i = 0; i < tasks.size(); ++i) {
              values[i].visibility = 1; // SH remains independent of scalar visibility.
              for (uint32_t c = 0; c < lightmap::irradianceLayers; ++c)
                values[i].irradiance[c] = direct[tasks[i].point][c][channel];
            }
            directionalCapture->observe(first, values, cancelled);
            offset += uint32_t(tasks.size());
          }
        }
        directionalCount = uint32_t(directionalPending.size());
        if (directionalCount) return;

      }
    } catch (const std::length_error &error) {
      std::fprintf(stderr, "Forward complete directional capture declined: %s\n", error.what());
      decline();
    } catch (const std::bad_alloc &) {
      std::fprintf(stderr, "Forward complete directional capture declined: host allocation\n");
      decline();
    }
  }

  void Controller::recordDirectionals(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (!directionalFieldsRequested() || !job.hasDirectDiffuse ||
        (job.directionalAttempted && !job.directionalCapture)) {
      recordSun(command, config);
      return;
    }
    // The controller checks the worker ticket before entering this method.
    // A window advances only after every submitted receipt has been consumed.
    if (!job.directionalAttempted || job.directionalReceived == job.directionalCount) {
      static std::atomic<uint64_t> generation = 0;
      const auto owner = m_job;
      const bool finish = bool(job.directionalCapture);
      job.ticket = m_pipelines->prepare("forward-directionals-" + std::to_string(++generation),
          [owner, finish] { owner->advanceDirectionals(finish); });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Encoding attributed surface lighting");
      return;
    }
    if (job.directionalNext < job.directionalCount) {
      CpuTime submissionTime{job.directionalTiming.submissionMs};
      auto &submission = m_submissions[m_slot];
      if (submission.job) throw std::logic_error("Directional window slot reused before its fence");
      const auto count = job.count(Work::Visibility, config,
          job.directionalCount - job.directionalNext, config.samples);
      std::vector<VisibilityPoint> points;
      points.reserve(count);
      for (const auto &task : job.directionalCapture->batch(std::span(job.directionalPending).subspan(job.directionalNext, count)))
        points.push_back({job.surfaceLayout.points[task.point], task.light});
      job.transport->recordVisibility(command, m_slot, points, config.samples, true);
      submission.job = m_job;
      submission.directionalField = true;
      submission.first = job.directionalNext;
      submission.count = count;
      job.directionalNext += count;
      job.directionalTracedPoints += count;
      ++job.directionalTiming.batches;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Baking attributed light fields %u-%u/%u (%u/%u samples)",
        job.directionalField + 1, job.directionalField + uint32_t(job.directionalCapture->window().size()),
        job.directionalFields, job.directionalReceived, job.directionalCount);
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
}
