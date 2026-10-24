#include "forward/bake/job.h"
#include "forward/probes/capture_dump.h"
#include "platform/workers.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace remix::forward::bake {
  bool Controller::recordLocalReflections(VkCommandBuffer command, const Config &config) {
    const char *enabled = std::getenv("REMIX_DEBUG_PROBE_LOCAL_CAPTURE");
    if (!enabled || std::strcmp(enabled, "1") != 0) return false;
    auto &job = *m_job;
    // Warm loads restore prepared images directly and need no capture transport.
    if (!job.reflectionEvidence || !job.transport || !job.prepared) return false;
    static std::atomic_uint64_t generation = 0;
    if (!job.localCapturePlanned) {
      job.localCapturePlanned = true;
      const auto owner = m_job;
      job.ticket = m_pipelines->prepare("forward-local-reflection-plan-" + std::to_string(++generation), [owner] {
        const auto baseline = probes::automaticCoverageBaseline(owner->reflectionImage->header());
        auto plan = probes::planReceiverCoverage(*owner->source, baseline, *owner->reflectionEvidence,
                                                 owner->cancellation.get_token());
        if (plan.replacements.empty()) return;
        owner->localCaptureWork = std::make_unique<probes::LocalCaptureWork>(*owner->source, baseline, plan,
            owner->reflectionImage->header().parameters[1], .5f / owner->source->sceneScale,
            owner->cancellation.get_token());
        owner->localReflections = std::make_shared<LocalReflections>();
        owner->localReflections->plan = std::make_shared<const probes::ReceiverCoveragePlan>(std::move(plan));
        std::fprintf(stderr, "Selected local reflection transport: %zu captures, %zu shared rays\n",
            owner->localReflections->plan->replacements.size(), owner->localCaptureWork->rays().size());
      });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Preparing selected local reflection captures");
      return true;
    }
    if (!job.localCaptureWork) return false;
    if (!job.localCaptureWork->complete()) {
      const auto rays = job.localCaptureWork->rays();
      if (job.localCaptureNext < rays.size()) {
        auto &submission = m_submissions[m_slot];
        if (submission.job) throw std::logic_error("Local reflection slot reused before its fence");
        const uint32_t count = job.count(Work::Reflection, config, uint32_t(rays.size() - job.localCaptureNext), config.samples, config.bounces);
        job.transport->recordRadiance(command, m_slot, rays.subspan(job.localCaptureNext, count), config.bounces, config.samples);
        submission.job = m_job;
        submission.localReflection = true;
        submission.first = job.localCaptureNext;
        submission.count = count;
        job.localCaptureNext += count;
      }
      status(REMIXAPI_BAKE_TRACING_EXT, "Baking selected local reflection captures");
      return true;
    }
    if (!job.localCaptureFiltered) {
      job.localCaptureFiltered = true;
      const auto owner = m_job;
      const auto physical = m_physical;
      const auto device = m_device;
      job.ticket = m_pipelines->prepare("forward-local-reflection-filter-" + std::to_string(++generation),
          [owner, physical, device, keepCache = config.diskCache, detailTolerance = config.reflectionDetailTolerance] {
        WorkerPool workers;
        auto prepared = owner->localCaptureWork->finish(128, owner->cancellation.get_token(), &workers);
        probes::dumpLocalImage(prepared.data);
        owner->localReflections->ready = std::make_shared<const probes::CoverageReadyImage>(std::move(prepared.selection));
        owner->localReflections->image = std::make_shared<probes::ImageAsset>(physical, device, prepared.data,
                                                                          owner->cancellation.get_token());
        if (keepCache && !owner->cacheFile.empty()) {
          auto snapshot = std::make_shared<local_cache::Snapshot>();
          snapshot->source = owner->source; snapshot->evidence = owner->reflectionEvidence;
          snapshot->detailTolerance = detailTolerance;
          snapshot->image.data = std::move(prepared.data);
          snapshot->image.selection = *owner->localReflections->ready;
          owner->localReflectionCache = std::move(snapshot);
        }
        owner->localCaptureWork.reset();
        std::fprintf(stderr, "Prepared local reflection image: %u layers, %llu bytes\n",
            owner->localReflections->image->header().parameters[0],
            static_cast<unsigned long long>(owner->localReflections->image->imageBytes()));
      });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Filtering selected local reflection captures");
      return true;
    }
    return false;
  }
  void Controller::Job::prepareLocalReflectionUpgrade(VkPhysicalDevice physical, VkDevice device,
      const Config &config, uint32_t family) {
    const char *enabled = std::getenv("REMIX_DEBUG_PROBE_LOCAL_CAPTURE");
    if (!enabled || std::strcmp(enabled, "1") || !cacheHit || localReflections) return;
    auto audit = probes::prepareReceiverAudit(*source, captures, cancellation.get_token());
    if (!audit) return; // No fresh diagnostic request: retain the ordinary cache-hit path.
    if (!transport) {
      Options options;
      options.queueFamily = family;
      options.batchPoints = Budget::maximumPoints;
      options.rayBudget = Budget::maximumWork;
      options.initialBatchPoints = 1024;
      options.textureResolution = config.textureResolution;
      options.lightingComponents = config.styledIndirect && !styleIds(*source).empty();
      auto preparedTransport = std::make_unique<Transport>();
      preparedTransport->initialize(physical, device, source, options);
      transport = std::move(preparedTransport);
    }
    receiverAudit = std::move(audit);
    localReflectionUpgrade = true;
    std::fprintf(stderr, "Preparing local reflection companion from cached lighting; diffuse transport skipped\n");
  }
  void Controller::Job::restoreLocalReflections(VkPhysicalDevice physical, VkDevice device, const Config &config) {
    const char *enabled = std::getenv("REMIX_DEBUG_PROBE_LOCAL_CAPTURE");
    if (!enabled || std::strcmp(enabled, "1") != 0 || !cacheHit || !reflectionImage || cacheFile.empty()) return;
    const auto baseline = probes::automaticCoverageBaseline(reflectionImage->header());
    auto cached = local_cache::read(local_cache::path(cacheFile), cacheKey, source, config, baseline,
                                    cancellation.get_token());
    if (!cached) return; // Base irradiance and cubemaps remain a valid cache hit.
    auto restored = std::make_shared<LocalReflections>();
    restored->image = std::make_shared<probes::ImageAsset>(physical, device, cached->image.data, cancellation.get_token());
    restored->ready = std::make_shared<const probes::CoverageReadyImage>(std::move(cached->image.selection));
    reflectionEvidence = std::move(cached->evidence);
    localReflections = std::move(restored);
    localCapturePlanned = localCaptureFiltered = true;
    std::fprintf(stderr, "Restored local reflection cache: %u layers, no capture rays\n",
        localReflections->image->header().parameters[0]);
  }

}
