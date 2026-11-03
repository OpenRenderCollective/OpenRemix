#include "forward/bake/job.h"
#include "forward/bake/transport.h"
#include "forward/bake/fingerprint.h"
#include "forward/bake/visibility_report.h"
#include "forward/probes/capture_cache.h"
#include "forward/probes/capture_dump.h"
#include "forward/probes/placement.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <stdexcept>

namespace remix::forward::bake {
  namespace { std::atomic<uint64_t> generation = 0; }

  // Source 2 preset adapter: reflection work owns its state machine; this
  // scheduler lends the default bake's transport and admission. The provider
  // owns its worker ticket. Preserve budget/order without another job or scene.
  void Controller::recordReflections(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (recordSunDepth(command, config)) return;
    if (job.reflectionRecovery && !job.publishedDiffuse) {
      job.publishedDiffuse = prepareVolume(command);
      m_volume = job.publishedDiffuse;
      m_reflectionSeeds = job.reflectionSeeds;
      m_submissions[m_slot].upload = m_volume;
    }
    using CaptureStep = probes::AutomaticCapture::Step;
    auto step = job.reflections.nextStep();
    auto dispatch = [&] {
      auto &submission = m_submissions[m_slot];
      if (submission.job) throw std::logic_error("Reflection slot reused before its fence");
      const probes::CaptureConfig execution{.samples = config.samples, .bounces = config.bounces,
                                            .batch = config.batch, .gpuBudgetMs = config.gpuBudgetMs};
      const auto batch = job.reflections.record(command, m_slot, *job.transport, job.budget, execution);
      if (!batch.count) return;
      submission.job = m_job;
      submission.first = batch.first;
      submission.count = batch.count;
      submission.reflection = batch.step;
    };
    if (step == CaptureStep::PlanSites) {
      const auto owner = m_job;
      job.reflections.prepare(
          *m_pipelines, "forward-probe-sites-" + std::to_string(++generation), [owner, config] {
            auto seeds = config.reflectionProbes
                ? owner->reflectionSeeds ? *owner->reflectionSeeds
                    : probes::placementSeeds(*owner->source, owner->grid, owner->cancellation.get_token())
                : probes::PlacementSeeds{};
            owner->reflections.prepareNext(*owner->source, {.requested = config.reflectionProbes},
                owner->cancellation.get_token(), std::move(seeds));
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Finding reflection capture sites");
      return;
    }
    if (step == CaptureStep::ValidateSites) {
      dispatch();
      const auto counts = job.reflections.progress(step);
      char progress[128];
      std::snprintf(progress, sizeof(progress), "Validating reflection capture sites (%u/%zu)",
                    counts.completed, counts.total);
      status(REMIXAPI_BAKE_TRACING_EXT, progress);
      return;
    }
    if (step == CaptureStep::Fit) {
      const auto owner = m_job;
      job.reflections.prepare(
          *m_pipelines, "forward-probe-placement-" + std::to_string(++generation), [owner, config] {
            owner->reflections.prepareNext(*owner->source,
                {.requested = config.reflectionProbes, .detailTolerance = config.reflectionDetailTolerance},
                owner->cancellation.get_token());
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Fitting reflection candidates");
      return;
    }
    if (step == CaptureStep::CheckGeometry) {
      dispatch();
      const auto counts = job.reflections.progress(step);
      char progress[128];
      std::snprintf(progress, sizeof(progress), "Checking reflection projection geometry (%u/%zu rays)",
                    counts.completed, counts.total);
      status(REMIXAPI_BAKE_TRACING_EXT, progress);
      return;
    }
    if (step == CaptureStep::Select) {
      const auto owner = m_job;
      VkPhysicalDeviceProperties limits;
      vkGetPhysicalDeviceProperties(m_physical, &limits);
      const uint32_t captureCount = std::min(config.reflectionProbes,
          probes::imageCapacity(config.reflectionResolution, limits.limits.maxImageDimension2D,
                                limits.limits.maxImageArrayLayers, limits.limits.maxStorageBufferRange));
      job.reflections.prepare(
          *m_pipelines, "forward-probe-selection-" + std::to_string(++generation), [owner, config, captureCount] {
            owner->reflections.prepareNext(*owner->source,
                {config.reflectionProbes, captureCount, config.reflectionResolution, config.reflectionDetailTolerance},
                owner->cancellation.get_token());
            owner->receiverAudit = probes::prepareReceiverAudit(*owner->source, owner->reflections.captureView(),
                                                               owner->cancellation.get_token());
            const probes::CaptureConfig captureConfig{.reflectionResolution = config.reflectionResolution,
                .textureResolution = config.textureResolution, .samples = config.samples, .bounces = config.bounces};
            const probes::capture_cache::Device cacheDevice{owner->cacheDevice.vendor, owner->cacheDevice.model,
                owner->cacheDevice.driver, {captureCount, config.reflectionResolution, owner->cacheDevice.imageSize,
                owner->cacheDevice.imageLayers, owner->cacheDevice.storageRange}, owner->cacheDevice.uuid};
            probes::capture_cache::Planned cacheRequest;
            if (!owner->cacheFile.empty() && !owner->reflections.captureView().empty())
              cacheRequest = {owner->cacheFile.parent_path(), owner->source->contentHash,
                  Transport::shaderFingerprint(), cacheDevice,
                  config.styledIndirect && !styleIds(*owner->source).empty(),
                  std::as_bytes(std::span(owner->captureDirectPolicy().lights))};
            owner->reflectionCacheKey = owner->reflections.prepareRadiance(captureConfig, cacheRequest,
                                                                          owner->cancellation.get_token());
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Selecting reflection captures");
      return;
    }
    if (recordReceiverDiagnostics(command, config)) return;
    step = job.reflections.nextStep();
    if (step == CaptureStep::Complete) {
      job.filtering = true;
      const auto owner = m_job;
      const auto physical = m_physical;
      const auto device = m_device;
      job.reflections.prepare(
          *m_pipelines, "forward-probe-filter-" + std::to_string(++generation), [owner, physical, device, config] {
            // Preserve exact input provenance on the worker, then release ray
            // scratch before filtered cubemaps, denoising and atlas encoding.
            // Publication must not scan this potentially large buffer again.
            owner->reflections.complete(owner->cancellation.get_token());
            const auto reflectionFingerprint = owner->reflectionRecovery ? ReflectionFingerprint{}
                : fingerprintCaptures(owner->reflections.captureView());
            if (!owner->reflectionRecovery) {
              dumpPhaseFingerprint("transport", owner->source->contentHash, owner->grid, owner->surfaceLayout, owner->surfaces,
                                   owner->sun, reflectionFingerprint);
              dumpVisibilityReport(*owner->mixedLights, owner->surfaceLayout,
                                    Options{}.bias / owner->source->sceneScale, owner->cancellation.get_token());
            }
            owner->finishStage.store(Job::FinishStage::Reflections, std::memory_order_relaxed);
            if (owner->inMemoryDiffuse && owner->reflectionCacheKey && !owner->reflections.cachedRadiance) {
              auto payload = owner->reflections.finishTransferredPublication(
                  physical, device, owner->cancellation.get_token());
              if (probes::capture_cache::retainedCharge(*payload) <= probes::capture_cache::maximumBytes)
                owner->reflectionPersistence = std::move(payload);
            } else
              owner->reflections.finishRetainedPublication(physical, device, owner->cancellation.get_token());
            if (!owner->reflectionRecovery)
              owner->finishDiffuseAssets(physical, device, config, reflectionFingerprint);
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Finishing baked lighting assets");
      return;
    }
    dispatch();
    const auto counts = job.reflections.progress(step);
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Capturing reflection cubemaps (%u/%zu texels, %u samples)",
                  counts.completed, counts.total, config.samples);
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
    return;
  }
  void Controller::publishReadyReflections(VkCommandBuffer command) {
    if (!m_job) return;
    const auto image = m_job->reflections.recordImageUpload(command);
    if (!image) return;
    m_reflections = image;
    m_reflectionSceneHash = m_job->source->contentHash;
    m_reflectionContent = m_job->reflections.content;
    m_submissions[m_slot].reflectionUpload = image;
  }
}
