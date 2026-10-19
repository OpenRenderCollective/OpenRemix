#include "forward/bake/job.h"
#include "forward/bake/storage_options.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <new>
#include <stdexcept>

namespace remix::forward::bake {
  void Controller::Job::advanceDirectionals(bool finish) {
    const auto cancelled = cancellation.get_token();
    const auto decline = [&] {
      directionalCapture.reset();
      directionalWords.clear();
      completeDirectionals = false;
    };
    try {
      if (!directionalAttempted) {
        directionalAttempted = true;
        const auto links = mixed::positiveDirectionalLinks(*mixedLights, cancelled);
        directionalFields = uint32_t(links.size());
        directionalCapture = std::make_unique<mixed::DirectionalCapture>(surfaceLayout,
            uint32_t(source->lights.size()), links, 256ull << 20,
            mixed::DirectionalFields::maximumBytes, cancelled);
      }
      for (;;) {
        if (finish) {
          if (!directionalCapture->finish(cancelled)) {
            std::fprintf(stderr, "Forward complete directional capture declined: encoded storage budget\n");
            decline();
            return;
          }
          ++directionalField;
        }
        if (directionalCapture->done()) {
          directionalWords = directionalCapture->words(cancelled);
          directionalCapture.reset();
          completeDirectionals = true;
          std::fprintf(stderr, "Forward complete directional capture: %u fields, %u reused, %llu traced points, %zu encoded bytes\n",
                       directionalFields, directionalReusedFields,
                       static_cast<unsigned long long>(directionalTracedPoints),
                       directionalWords.size() * sizeof(uint32_t));
          return;
        }
        directionalCount = directionalCapture->begin(cancelled);
        directionalNext = directionalReceived = 0;
        finish = true;
        if (!directionalCount) continue;
        // Existing retained fields already have the same unit-radiance HWRT
        // samples. Reuse them rather than tracing the first four owners again.
        const auto first = directionalCapture->batch(0, 1).front();
        const auto chart = surfaceLayout.pointCharts[first.point];
        const auto &owners = mixedLights->directCharts[chart];
        const auto owner = std::find(owners.begin(), owners.end(), first.light);
        if (owner == owners.end() || direct.empty()) return;
        const auto channel = size_t(owner - owners.begin());
        ++directionalReusedFields;
        constexpr uint32_t batchSize = 4096;
        for (uint32_t offset = 0; offset < directionalCount;) {
          const auto tasks = directionalCapture->batch(offset, std::min(batchSize, directionalCount - offset));
          std::vector<VisibilityResult> values(tasks.size());
          for (size_t i = 0; i < tasks.size(); ++i) {
            values[i].visibility = 1; // SH is independently retained, including black fields.
            for (uint32_t c = 0; c < lightmap::irradianceLayers; ++c)
              values[i].irradiance[c] = direct[tasks[i].point][c][channel];
          }
          directionalCapture->observe(offset, values, cancelled);
          offset += uint32_t(tasks.size());
        }
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
    // A field advances only after every submitted receipt has been consumed.
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
      auto &submission = m_submissions[m_slot];
      if (submission.job) throw std::logic_error("Directional field slot reused before its fence");
      const auto count = job.count(Work::Visibility, config,
          job.directionalCount - job.directionalNext, config.samples);
      std::vector<VisibilityPoint> points;
      points.reserve(count);
      for (const auto &task : job.directionalCapture->batch(job.directionalNext, count))
        points.push_back({job.surfaceLayout.points[task.point], task.light});
      job.transport->recordVisibility(command, m_slot, points, config.samples, true);
      submission.job = m_job;
      submission.directionalField = true;
      submission.first = job.directionalNext;
      submission.count = count;
      job.directionalNext += count;
      job.directionalTracedPoints += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Baking attributed light %u/%u (%u/%u samples)",
        job.directionalField + 1, job.directionalFields, job.directionalReceived, job.directionalCount);
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
}
