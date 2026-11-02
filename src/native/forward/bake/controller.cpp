#include "forward/bake/job.h"
#include "forward/probes/capture_cache.h"
#include "forward/bake/model_charts.h"
#include "forward/bake/model_preflight.h"
#include "forward/probes/priority_bands.h"
#include "forward/bake/storage_options.h"
#include "forward/bake/budget.h"
#include "forward/bake/cache.h"
#include "forward/bake/cache_persistence.h"
#include "forward/bake/fingerprint.h"
#include "forward/bake/visibility_report.h"
#include "forward/bake/receiver_report.h"
#include "forward/bake/receiver_support.h"
#include "forward/bake/transport.h"
#include "forward/image_dump.h"
#include "forward/bake/sun_depth.h"
#include "forward/lightmap/interior_donors.h"
#include "forward/mixed/overflow.h"
#include "forward/probes/capture_dump.h"
#include "forward/probes/data.h"
#include "forward/probes/placement.h"
#include "raster/renderer.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

namespace remix::forward::bake {
  namespace {
    std::atomic<uint64_t> generation = 0;
    lightmap::Options surfaceAtlasOptions(const Config &config, const VkPhysicalDeviceLimits &limits) {
      lightmap::Options atlas;
      atlas.padding = 0;
      atlas.pageSize = config.lightmapPageSizeForBudget();
      atlas.luxelMetres = config.lightmapLuxelSize;
      atlas.fillBudget = config.lightmapFitBudget;
      atlas.maxTexels = config.lightmapTexels;
      atlas.maxPages = std::min(256u, limits.maxImageArrayLayers / lightmap::atlasLayers);
      if (atlas.pageSize > limits.maxImageDimension2D)
        throw std::invalid_argument("Surface lightmap page exceeds device limit");
      return atlas;
    }


  } // namespace
  bool Controller::Job::consumeVisibility(uint32_t slot, uint32_t first, uint32_t count, Visibility kind) {
    if (kind == Visibility::Local) {
      overflowReadback.resize(count);
      transport->readScalarVisibility(slot, overflowReadback);
      for (uint32_t i = 0; i < count; ++i) {
        if (!std::isfinite(overflowReadback[i])) return false;
        const auto &task = mixedLights->tasks[first + i];
        visibility[task.point][task.channel] = std::clamp(overflowReadback[i], 0.f, 1.f);
      }
      return true;
    }
    if (kind == Visibility::ProbeLocal) {
      if (!localProbes) return false;
      try {
        const auto tasks = std::span(localProbeTasks).subspan(first, count);
        if (localProbes->directional()) {
          visibilityReadback.resize(count);
          transport->readVisibility(slot, visibilityReadback);
          localProbes->observe(tasks, visibilityReadback);
        } else {
          overflowReadback.resize(count);
          transport->readScalarVisibility(slot, overflowReadback);
          localProbes->observe(tasks, overflowReadback);
        }
        return true;
      } catch (const std::invalid_argument &) {
        return false;
      }
    }
    if (kind == Visibility::Overflow) {
      if (overflowReadback.size() < count)
        overflowReadback.resize(count);
      const auto values = std::span(overflowReadback).first(count);
      transport->readScalarVisibility(slot, values);
      for (uint32_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i]))
          return false;
        overflowAudit->observe(first + i, std::clamp(values[i], 0.f, 1.f), surfaceLayout);
      }
      return true;
    }
    if (visibilityReadback.size() < count)
      visibilityReadback.resize(count);
    const auto results = std::span(visibilityReadback).first(count);
    transport->readVisibility(slot, results);
    if (kind == Visibility::DirectionalField) {
      if (!directionalCapture) return false;
      try {
        directionalCapture->observe(std::span(directionalPending).subspan(first, count), results, cancellation.get_token());
        return true;
      } catch (const std::invalid_argument &) {
        return false;
      }
    }
    const auto directBatch = kind == Visibility::Direct ? directWork->batch(first, count)
                                                       : std::vector<VisibilityTask>{};
    for (uint32_t i = 0; i < count; ++i) {
      const auto &value = results[i];
      if (!std::isfinite(value.visibility) ||
          !std::all_of(value.irradiance.begin(), value.irradiance.end(),
                       [](float coefficient) { return std::isfinite(coefficient); }))
        return false;
      if (kind == Visibility::ProbeSun) {
        auto &probe = grid.probes[probeSunTasks[first + i]];
        // RGB and relocation offsets remain unchanged. Only usable irradiance
        // probes can advertise matching static sun visibility.
        probe.irradiance[7][3] = probe.statistics[3] > .5f ? 1.f : 0.f;
        probe.irradiance[8][3] = std::clamp(value.visibility, 0.f, 1.f);
      } else if (kind == Visibility::Direct) {
        const auto &task = directBatch[i];
        for (uint32_t coefficient = 0; coefficient < lightmap::irradianceLayers; ++coefficient)
          direct[task.point][coefficient][task.channel] = value.irradiance[coefficient];
      } else {
        const auto point = kind == Visibility::SurfaceSunAntialiasing ? sunAntialiasing->receiver(first + i)
                                                                    : first + i;
        auto &output = sun[point];
        output.visibility = std::clamp(value.visibility, 0.f, 1.f);
        output.direct = value.irradiance;
      }
    }
    return true;
  }
  void Controller::Job::prepareResources(VkPhysicalDevice physical, VkDevice device, const Config &config,
                                         float lightCutoff, uint32_t queueFamily) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    if (const char *path = std::getenv("REMIX_DEBUG_BAKE_MODEL_ATLAS_PREFLIGHT"); path && *path) {
      writeModelAtlasPreflight(*source, surfaceAtlasOptions(config, properties.limits),
                              std::filesystem::u8path(path), cancellation.get_token());
      throw std::runtime_error("Model atlas preflight complete (diagnostic only; no bake scheduled)");
    }
    if (const char *path = std::getenv("REMIX_DEBUG_BAKE_MODEL_PREFLIGHT"); path && *path) {
      writeModelPreflight(*source, std::filesystem::u8path(path), cancellation.get_token());
      throw std::runtime_error("Model eligibility preflight complete (diagnostic only; no bake scheduled)");
    }
    // Initial preparation owns this job exclusively until its ticket completes.
    // Keep caller identity/bindings intact and derive only private atlas inputs.
    if (config.modelLightmaps) {
      if (!config.lightmapFitBudget)
        throw std::invalid_argument("Model charts require world-metric budget fitting");
      const auto capacity = modelChartCapacity(surfaceAtlasOptions(config, properties.limits), source->charts.size());
      if (!capacity) throw std::length_error("No model chart capacity in surface atlas");
      auto charts = modelCharts(*source, capacity, cancellation.get_token());
      auto derived = std::make_shared<scene::BakeScene>(*source);
      derived->charts.insert(derived->charts.end(), charts.begin(), charts.end());
      std::fprintf(stderr, "Forward+ model charts: authored %zu, generated %zu\n",
                   source->charts.size(), charts.size());
      source = std::move(derived);
    }

    const auto directLighting = (config.aggregateDiffuse || config.fullyBakedLights || config.bakedGeometryLights)
        ? staticAggregatePolicy(*source, config.fullyBakedLights ? DirectMode::Baked : DirectMode::BakedMixed,
                                 config.bakedGeometryLights)
        : DirectPolicy{};
    const char *samples = std::getenv("REMIX_DEBUG_BAKE_SURFACE_SAMPLES");
    reference = ReferenceCapture::fromEnvironment();
    const bool depthCapture = std::getenv("REMIX_DEBUG_BAKE_SUN_DEPTH") != nullptr;
    if (!coarseTransferPreparationRequested() && !reference && config.diskCache && (!samples || std::strcmp(samples, "1") != 0)) {
      cache::Device deviceIdentity{properties.vendorID, properties.deviceID, properties.driverVersion,
                                     properties.limits.maxImageDimension2D,
                                     properties.limits.maxImageArrayLayers,
                                     properties.limits.maxStorageBufferRange,
                                     {}};
      std::copy_n(properties.pipelineCacheUUID, deviceIdentity.uuid.size(), deviceIdentity.uuid.begin());
      const bool fallbackReflections = !source->partition || !source->partition->closed;
      cacheDevice = deviceIdentity;
      cacheKey = cache::key(source->contentHash, config, lightCutoff, deviceIdentity, directLighting,
                            fallbackReflections);
      try {
        cacheFile = cache::path(cacheKey);
      } catch (const std::exception &) {
        cacheFile.clear();
      }
      if (!cacheFile.empty()) {
        auto cached = cache::read(cacheFile, cacheKey, source, config, deviceIdentity,
                                  cancellation.get_token(), directLighting, cache::ReflectionRead::RecoverMissing);
        if (cached && directionalFieldsRequested() && cached->directDiffuse &&
            (!cached->directionalAttempted || (cached->directionals.empty() &&
             cached->directionalBudget < mixed::directionalStorageBudget(cached->layout))))
          cached.reset(); // Upgrade only a missing capability or a smaller declined budget.
        if (cached && depthCapture) {
          sunDepth = SunDepth::prepare(physical, device, *source, cached->layout, cancellation.get_token(),
                                       true, config.textureResolution);
        }
        if (!cancellation.stop_requested())
          cache::recordLookup(cacheFile, cacheKey, source->contentHash,
                               cache::key(0, config, lightCutoff, deviceIdentity, directLighting,
                                          fallbackReflections), bool(cached));
        if (cached) {
          reflectionRecovery = cached->missingReflections;
          grid = std::move(cached->grid);
          surfaceLayout = std::move(cached->layout);
          mixedLights = std::make_shared<mixed::Plan>(std::move(cached->mixed));
          reflections.captures = std::move(cached->captures);
          reflections.filtered = std::move(cached->specular);
          reflections.restoreProjection(*source, config.reflectionDetailTolerance, cancellation.get_token());
          surfacePixels = std::move(cached->pixels);
          styleWords = std::move(cached->styles);
          localProbes = std::move(cached->localProbes);
          directionalAttempted = cached->directionalAttempted;
          directionalBudget = cached->directionalBudget;
          completeDirectionals = !cached->directionals.empty();
          directionalWords = std::move(cached->directionals);
          surfaceReceived = cached->surfaceSamples;
          hasDirectDiffuse = cached->directDiffuse;
          hasSun = cached->sun;
          received = uint32_t(grid.tasks.size());
          reflections.inputHash = probes::CaptureWork::inputHash(reflections.captureView(), cancellation.get_token());
          if (!source->charts.empty())
            surfaceGeometry = std::make_shared<lightmap::Geometry>(
                physical, device, source, surfaceLayout, cancellation.get_token(),
                mixedLights->charts, mixedLights->directCharts);
          dumpReceiverReport(*source, surfaceLayout, surfacePixels, grid, cancellation.get_token());
          prepareSurfaceStorage();
          prepareGroupedStyles(properties.limits.maxStorageBufferRange, config);
          if (reflectionRecovery || (sunDepth && !sunDepth->restored())) {
            // Missing auxiliary components must not retrace cached irradiance/styles.
            Options depthOptions;
            depthOptions.depthCapture = bool(sunDepth);
            if (reflectionRecovery) {
              depthOptions.directLighting = directLighting;
              depthOptions.lightingComponents = config.styledIndirect && !styleIds(*source).empty();
            }
            depthOptions.batchPoints = Budget::maximumPoints;
            depthOptions.rayBudget = Budget::maximumWork;
            depthOptions.initialBatchPoints = 1024;
            depthOptions.queueFamily = queueFamily;
            depthOptions.textureResolution = config.textureResolution;
            transport = std::make_unique<Transport>();
            transport->initialize(physical, device, source, depthOptions);
          }
          cacheHit = true;
          filtering = !reflectionRecovery;
          return;
        }
      }
    }
    if (cancellation.stop_requested())
      throw std::runtime_error("Bake preparation superseded");
    auto placement = config;
    const uint64_t directoryReserve = source->partition && source->partition->closed
        ? uint64_t(maximumDirectoryWords) * sizeof(uint32_t) : 0;
    if (properties.limits.maxStorageBufferRange <= sizeof(GridHeader) + directoryReserve +
                                                   (config.directionalProbeDepth ? 16 : 0))
      throw std::invalid_argument("Irradiance directory exceeds device storage range");
    placement.maxProbes =
        std::min(placement.maxProbes,
                 uint32_t((properties.limits.maxStorageBufferRange - sizeof(GridHeader) - directoryReserve -
                           (config.directionalProbeDepth ? 16 : 0)) /
                          (sizeof(Result) + (config.directionalProbeDepth ? sizeof(ProbeDepth::Record) : 0))));
    const auto atlas = surfaceAtlasOptions(config, properties.limits);
    surfaceLayout = lightmap::fit(*source, atlas, cancellation.get_token());
    grid = placeRefinementBase(*source, placement, surfaceLayout, refinement, cancellation.get_token());
    finalProbePass = !refinement.factor;
    if (std::getenv("REMIX_DEBUG_BAKE_GRID_PREFIX")) {
      const auto support = receiverSupport(grid.header, surfaceLayout, 0, cancellation.get_token());
      probes::dumpPublishedData(".receiver-support.bin", support.data(), support.size());
      const auto halo = receiverSupport(grid.header, surfaceLayout, 3 / source->sceneScale,
                                        cancellation.get_token());
      probes::dumpPublishedData(".receiver-halo.bin", halo.data(), halo.size());
      probes::dumpPublishedData(".receiver-grid-header.bin", &grid.header, sizeof(grid.header));
      probes::dumpPublishedData(".receiver-grid-lookup.bin", grid.lookup.data(), grid.lookup.size() * sizeof(uint32_t));
    }
    if (depthCapture && !sunDepth) sunDepth = SunDepth::prepare(physical, device, *source, surfaceLayout, cancellation.get_token(),
                                                            config.diskCache, config.textureResolution);
    surfaces.resize(surfaceLayout.points.size());
    Options options;
    options.directLighting = directLighting;
    options.depthCapture = bool(sunDepth) || coarseTransferPreparationRequested();
    mixedLights = std::make_shared<mixed::Plan>(
        mixed::plan(source, surfaceLayout, lightCutoff, cancellation.get_token(),
                    options.bias / source->sceneScale, directLighting));
    if (std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX")) {
      uint64_t candidates = mixedLights->overflow;
      for (const auto &chart : mixedLights->charts)
        for (auto light : chart)
          candidates += light != mixed::none;
      std::fprintf(stderr, "OpenRemix mixed plan: candidates=%llu hemisphere_rejected=%llu cone_rejected=%llu initial_tasks=%zu\n",
                   static_cast<unsigned long long>(candidates),
                   static_cast<unsigned long long>(mixedLights->hemisphereRejectedPairs),
                   static_cast<unsigned long long>(mixedLights->coneRejectedPairs), mixedLights->tasks.size());
    }
    visibility.resize(surfaceLayout.points.size(), {1, 1, 1, 1});
    if (source->sun.isDistant)
      sun.resize(surfaceLayout.points.size());
    if (config.directDiffuse && !mixedLights->tasks.empty())
      direct.resize(surfaceLayout.points.size());
    hasSun = !sun.empty();
    hasDirectDiffuse = config.directDiffuse && (!direct.empty() || hasSun);
    options.batchPoints = Budget::maximumPoints;
    options.rayBudget = Budget::maximumWork;
    options.initialBatchPoints = 1024;
    options.queueFamily = queueFamily;
    options.textureResolution = config.textureResolution;
    options.lightingComponents = config.styledIndirect && !styleIds(*source).empty();
    if (reference) {
      reference->sceneHash = source->contentHash;
      reference->classify(*source);
      reference->bounces = reference->geometry ? 1 : config.bounces;
      reference->maximumGeometryDistance = options.distance / source->sceneScale;
      reference->analyticReceiverDirectPolicy =
          !reference->geometry && !reference->incomingIrradiance && !reference->usesRadianceRays() &&
          (config.aggregateDiffuse || config.fullyBakedLights || config.bakedGeometryLights);
      reference->combinedLighting = !reference->geometry && options.lightingComponents && !reference->baseLightingOnly;
    }
    transport = std::make_unique<Transport>();
    transport->initialize(physical, device, source, options);
    const char *cpuVisibility = std::getenv("REMIX_DEBUG_BAKE_CPU_VISIBILITY");
    if ((!mixedLights->tasks.empty() || mixedLights->overflow) &&
        (!cpuVisibility || std::strcmp(cpuVisibility, "1") != 0))
      transport->prepareVisibilityReceivers(surfaceLayout.points);
  }
  Controller::Controller(VkPhysicalDevice physical, VkDevice device, pipeline::Service *pipelines,
                         bool hardware, uint32_t queueFamily)
      : m_physical(physical), m_device(device), m_pipelines(pipelines), m_queueFamily(queueFamily) {
    m_empty = std::make_unique<Buffer>();
    m_empty->create(physical, device, sizeof(probes::GpuHeader), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const probes::GpuHeader empty{};
    m_empty->write(&empty, sizeof(empty));
    m_emptySunDepth = std::make_unique<Buffer>();
    m_emptySunDepth->create(physical, device, sizeof(SunDepthHeader), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const SunDepthHeader emptyDepth{};
    m_emptySunDepth->write(&emptyDepth, sizeof(emptyDepth));
    m_emptyReflections = std::make_unique<probes::ImageAsset>(physical, device, probes::Data{});
    m_emptySurface = std::make_unique<Image>();
    m_emptySurface->create(physical, device, {1, 1}, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                           lightmap::atlasLayers);
    m_statistics.structSize = sizeof(m_statistics);
    m_emptyOcclusion = std::make_unique<mixed::OcclusionMap>(physical, device, VkExtent2D{1, 1});
    m_statistics.supported = hardware && pipelines;
  }
  Controller::~Controller() {
    if (m_job)
      m_job->cancellation.request_stop();
    if (m_job && m_job->ticket)
      m_pipelines->cancel(m_job->ticket);
    for (auto &job : m_obsolete)
      if (job->ticket)
        m_pipelines->cancel(job->ticket);
  }
  void Controller::status(uint32_t state, const char *message) {
    static const bool timing = [] {
      const auto value = std::getenv("REMIX_DEBUG_BAKE_TIMING");
      return value && std::strcmp(value, "1") == 0;
    }();
    if (timing && (m_statistics.state != state ||
                   std::strncmp(m_statistics.status, message, sizeof(m_statistics.status) - 1) != 0)) {
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      // Join progress with frame/GPU telemetry without changing scheduling or
      // bake identity. Repeated unchanged status does not generate log traffic.
      std::fprintf(stderr, "Bake progress: unix_ms=%lld state=%u scene=%llu status=%.*s\n",
                   static_cast<long long>(now), state,
                   static_cast<unsigned long long>(m_source ? m_source->sceneId : 0),
                   int(sizeof(m_statistics.status) - 1), message);
    }
    m_statistics.state = state;
    std::snprintf(m_statistics.status, sizeof(m_statistics.status), "%s", message);
  }
  void Controller::invalidate() {
    if (m_job) {
      m_job->cancellation.request_stop();
      if (m_job->ticket && !m_job->ticket->ready()) {
        m_pipelines->cancel(m_job->ticket);
        m_obsolete.push_back(m_job);
      }
      m_job.reset();
    }
    m_volume.reset();
    m_reflections.reset();
    m_reflectionSceneHash = 0;
    m_statistics.probes = m_statistics.completed = 0;
  }
  void Controller::selectFrame(uint32_t slot) {
    // The caller completed this slot's fence. Only now can readback buffers
    // be consumed and resources referenced by that submission be released.
    m_slot = slot;
    auto &submission = m_submissions.at(slot);
    if (m_emptyReflectionUpload == slot) {
      m_emptyReflections->releaseUpload();
      m_emptyReflectionUpload = ~0u;
    }
    if (submission.upload) {
      submission.upload->staging.reset();
      if (submission.upload->sunDepth) submission.upload->sunDepth->releaseUpload();
    }
    if (submission.reflectionUpload) submission.reflectionUpload->releaseUpload();
    if (submission.localReflectionUpload) submission.localReflectionUpload->releaseUpload();
    if (submission.job) {
      auto &job = *submission.job;
      if (submission.sunDepthCache && submission.job == m_job) {
        job.sunDepthCacheReady = true;
        job.readbackBytes += job.sunDepth->depthBytes();
      }
      if (submission.preparation) {
        job.transport->releaseBuildInputs();
        if (job.sunDepth && job.sunDepth->restored()) job.sunDepth->releaseUpload();
      }
      if (submission.count && submission.job == m_job && !job.failed) {
        const auto readBegin = std::chrono::steady_clock::now();
        if (submission.sunDepth) {
          // Device-only output: the owning slot fence is the completion receipt.
        } else if (submission.coarseTransfer) {
          std::vector<float> depths(submission.count);
          job.coarseTransferSlots[slot].readback->read(depths.data(), depths.size()*sizeof(float));
          job.coarseTransfer->consume(submission.first, depths);
          job.coarseTransferReceived += submission.count;
        } else if (submission.probeDepth) {
          job.readback.resize(submission.count);
          job.transport->read(slot, job.readback);
          for (uint32_t i = 0; i < submission.count; ++i)
            job.grid.depth.records[job.grid.tasks[submission.first + i]] =
                std::bit_cast<ProbeDepth::Record>(job.readback[i]);
          job.probeDepthReceived += submission.count;
        } else if (submission.reference) {
          job.readback.resize(submission.count);
          job.transport->read(slot, job.readback);
          job.reference->consume(submission.first, job.readback);
        } else if (submission.visibility || submission.direct || submission.directionalField || submission.sun ||
            submission.probeSun || submission.probeLocal || submission.overflowVisibility) {
          const auto kind = submission.probeLocal ? Job::Visibility::ProbeLocal
                          : submission.probeSun ? Job::Visibility::ProbeSun
                          : submission.overflowVisibility ? Job::Visibility::Overflow
                          : submission.visibility ? Job::Visibility::Local
                          : submission.directionalField ? Job::Visibility::DirectionalField
                          : submission.direct ? Job::Visibility::Direct
                          : submission.sunAntialiasing ? Job::Visibility::SurfaceSunAntialiasing
                          : Job::Visibility::SurfaceSun;
          if (!job.consumeVisibility(slot, submission.first, submission.count, kind)) {
            job.failed = true;
            status(REMIXAPI_BAKE_FAILED_EXT, "Bake produced non-finite visibility");
          }
        } else {
          if (job.readback.size() < submission.count)
            job.readback.resize(submission.count);
          const auto results = std::span(job.readback).first(submission.count);
          job.transport->read(slot, results);
          if (submission.styled) {
            try {
              if (!job.styles) throw std::logic_error("Attributed readback has no owner");
              job.styles->consume(submission.styled, submission.surface, submission.first, results);
            } catch (const std::exception &) {
              job.failed = true;
              status(REMIXAPI_BAKE_FAILED_EXT, "Invalid attributed irradiance readback");
            }
          } else if (submission.localReflection) {
            if (!job.localCaptureWork || !job.localCaptureWork->observe(submission.first, results)) {
              job.failed = true;
              status(REMIXAPI_BAKE_FAILED_EXT, "Invalid selected reflection radiance readback");
            }
          } else if (submission.receiverGuardCoverage) {
            if (!job.receiverGuardCoverage || !job.receiverGuardCoverage->observe(submission.first, results)) {
              job.failed = true;
              status(REMIXAPI_BAKE_FAILED_EXT, "Invalid reflection guard readback ownership");
            }
          } else if (submission.receiverFitCoverage) {
            if (!job.receiverFitCoverage || !job.receiverFitCoverage->observe(submission.first, results)) {
              job.failed = true;
              status(REMIXAPI_BAKE_FAILED_EXT, "Invalid local coverage readback ownership");
            }
          } else if (submission.receiverFit) {
            if (!job.receiverFits || !job.receiverFits->observe(submission.first, results)) {
              job.failed = true;
              status(REMIXAPI_BAKE_FAILED_EXT, "Invalid local projection readback ownership");
            }
          } else if (submission.receiverSiteCheck) {
            if (!job.receiverSiteChecks || !job.receiverSiteChecks->observe(submission.first, results)) {
              job.failed = true;
              status(REMIXAPI_BAKE_FAILED_EXT, "Invalid receiver site check ownership");
            }
          } else if (submission.receiverSite) {
            if (!job.receiverSites || !job.receiverSites->observe(submission.first, results)) {
              job.failed = true;
              status(REMIXAPI_BAKE_FAILED_EXT, "Invalid receiver site readback ownership");
            }
          } else if (submission.reflectionReceiver) {
            if (!job.receiverAudit || !job.receiverAudit->observe(submission.first, results)) {
              job.failed = true;
              status(REMIXAPI_BAKE_FAILED_EXT, "Invalid reflection receiver readback ownership");
            }
          } else if (submission.reflection != probes::CaptureStep::None) {
            if (!job.reflections.observe({submission.reflection, submission.first, submission.count}, results)) {
              job.failed = true;
              status(REMIXAPI_BAKE_FAILED_EXT, "Invalid automatic reflection readback");
            }
          } else {
            for (uint32_t i = 0; i < submission.count; ++i) {
              auto &value = results[i];
              bool finite = true;
              for (const auto &coefficient : value.irradiance)
                for (float channel : coefficient)
                  finite = finite && std::isfinite(channel);
              if (!finite) {
                job.failed = true;
                status(REMIXAPI_BAKE_FAILED_EXT, "Bake produced non-finite irradiance");
              }
              if (submission.surface) {
                job.surfaces[submission.first + i] = value;
              } else {
                value.statistics[3] = !job.failed && value.statistics[0] < .5f ? 1.f : 0.f;
                storeProbe(job.grid, job.grid.tasks[submission.first + i], value);
              }
            }
          }
        }
        const double readbackMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readBegin).count();
        job.readbackMs += readbackMs;
        if (submission.coarseTransfer) job.coarseTransferReadbackMs += readbackMs;
        if (submission.directionalField) job.directionalTiming.readbackMs += readbackMs;
        if (submission.overflowVisibility)
          job.overflowTiming.readbackMs += readbackMs;
        job.readbackBytes += uint64_t(submission.count) *
                             (submission.sunDepth ? 0 : submission.coarseTransfer || submission.overflowVisibility || submission.visibility ||
                              (submission.probeLocal && !job.localProbes->directional()) ? sizeof(float) :
                              submission.direct || submission.directionalField || submission.sun ||
                              submission.probeSun || submission.probeLocal
                                  ? sizeof(VisibilityResult) : sizeof(Result));
        if (!submission.styled && !submission.reference && !submission.probeDepth && !submission.coarseTransfer &&
            submission.reflection == probes::CaptureStep::None) (submission.sunDepth ? job.sunDepthReceived
         : submission.localReflection ? job.localCaptureReceived
         : submission.receiverGuardCoverage ? job.receiverGuardReceived
         : submission.receiverFitCoverage ? job.receiverCoverageReceived
         : submission.receiverFit ? job.receiverFitReceived
         : submission.receiverSiteCheck ? job.receiverCheckReceived
         : submission.receiverSite ? job.receiverSiteReceived
         : submission.reflectionReceiver ? job.receiverReceived
         : submission.visibility         ? job.visibilityReceived
         : submission.directionalField   ? job.directionalReceived
         : submission.direct             ? job.directReceived
         : submission.overflowVisibility ? job.overflowReceived
         : submission.probeSun           ? job.probeSunReceived
         : submission.probeLocal         ? job.localProbeReceived
         : submission.sunAntialiasing     ? job.sunAntialiasingReceived
         : submission.sun                ? job.sunReceived
         : submission.surface            ? job.surfaceReceived
                                         : job.received) += submission.count;
        if (const auto elapsed = job.transport->elapsed(slot)) {
          if (submission.sunDepth) job.sunDepthGpuMs += *elapsed;
          if (submission.coarseTransfer) job.coarseTransferGpuMs += *elapsed;
          if (submission.directionalField) job.directionalTiming.gpuMs += *elapsed;
          if (submission.sunAntialiasing) job.sunAntialiasingGpuMs += *elapsed;
          if (submission.overflowVisibility)
            job.overflowTiming.gpuMs += *elapsed;
          const auto work = submission.reflection != probes::CaptureStep::None
                            ? probes::AutomaticCapture::workType(submission.reflection)
                            : submission.coarseTransfer                                ? Work::CoarseTransfer
                            : submission.probeDepth                                  ? Work::ProbeDepth
                            : submission.reflectionReceiver || submission.receiverSiteCheck || submission.receiverFit || submission.receiverFitCoverage || submission.receiverGuardCoverage || submission.sunDepth ? Work::Geometry
                            : submission.visibility || submission.direct || submission.directionalField ||
                              submission.overflowVisibility || submission.probeLocal     ? Work::Visibility
                            : submission.sun || submission.probeSun                  ? Work::Sun
                            : submission.receiverSite  ? Work::Site
                            : submission.localReflection      ? Work::Reflection
                            : submission.surface                                     ? Work::Surface
                                                                                     : Work::Irradiance;
          if (!submission.reference)
            job.budget.observe(work, submission.count, *elapsed, m_config.gpuBudgetMs);
        }
        m_statistics.completed = job.received;
      }
    }
    submission = {};
  }
  void Controller::recordSurfaces(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (job.surfaceReceived == job.surfaceLayout.points.size()) {
      if (!job.cacheHit && !job.surfaceDonorsResolved && !job.surfaces.empty()) {
        job.surfaceDonorsResolved = true;
        const auto owner = m_job;
        job.ticket = m_pipelines->prepare(
            "forward-surface-donors-" + std::to_string(++generation), [owner] {
              const auto began = std::chrono::steady_clock::now();
              const auto repaired = lightmap::repairInteriorDonors(
                  owner->surfaceLayout, owner->surfaces, owner->cancellation.get_token());
              std::fprintf(stderr, "Forward surface interior donors: %u hidden samples, %u repaired texels, "
                                   "%u charts without an exterior donor, %.3f ms CPU\n",
                           repaired.hiddenSamples, repaired.repairedTexels, repaired.unresolvedCharts,
                           std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - began).count());
            });
        status(REMIXAPI_BAKE_PREPARING_EXT, "Resolving covered surface lightmap samples");
        return;
      }
      recordVisibility(command, config);
      return;
    }
    if (job.surfaceNext < job.surfaceLayout.points.size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Surface bake slot reused before its fence");
      const uint32_t count =
          job.count(Work::Surface, config, uint32_t(job.surfaceLayout.points.size() - job.surfaceNext),
                    config.samples, config.bounces);
      job.transport->recordBatch(command, m_slot,
                                 std::span(job.surfaceLayout.points).subspan(job.surfaceNext, count),
                                 config.samples, config.bounces);
      submission.job = m_job;
      submission.surface = true;
      submission.first = job.surfaceNext;
      submission.count = count;
      job.surfaceNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Baking surface lightmaps (%u/%zu texels)", job.surfaceReceived,
                  job.surfaceLayout.points.size());
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
  void Controller::recordProbeSun(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (!job.probeSunPlanned) {
      if (job.source->sun.isDistant)
        for (const auto index : job.grid.tasks)
          if (job.grid.probes[index].statistics[3] > .5f)
            job.probeSunTasks.push_back(index);
      job.probeSunPlanned = true;
    }
    if (job.probeSunReceived == job.probeSunTasks.size()) {
      recordProbeLocal(command, config);
      return;
    }
    if (job.probeSunNext < job.probeSunTasks.size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Probe sunlight slot reused before its fence");
      const uint32_t count = job.count(Work::Sun, config,
          uint32_t(job.probeSunTasks.size() - job.probeSunNext), config.samples);
      std::vector<Point> points;
      points.reserve(count);
      for (uint32_t i = 0; i < count; ++i)
        points.push_back(job.grid.points[job.probeSunTasks[job.probeSunNext + i]]);
      // Probe points have no surface normal, so transport applies no surface
      // offset. Relocated positions match those used by indirect irradiance.
      job.transport->recordSun(command, m_slot, points, config.samples, false);
      submission.job = m_job;
      submission.probeSun = true;
      submission.first = job.probeSunNext;
      submission.count = count;
      job.probeSunNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Baking probe sunlight visibility (%u/%zu probes)",
                  job.probeSunReceived, job.probeSunTasks.size());
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
  void Controller::recordSun(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (job.sunReceived == job.sun.size()) {
      recordSunAntialiasing(command, config);
      return;
    }
    if (job.sunNext < job.sun.size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Sun visibility slot reused before its fence");
      const uint32_t count =
          job.count(Work::Sun, config, uint32_t(job.sun.size() - job.sunNext), config.samples);
      job.transport->recordSun(command, m_slot,
                               std::span(job.surfaceLayout.points).subspan(job.sunNext, count),
                               config.samples, config.directDiffuse);
      submission.job = m_job;
      submission.sun = true;
      submission.first = job.sunNext;
      submission.count = count;
      job.sunNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Baking static sunlight (%u/%zu texels)", job.sunReceived,
                  job.sun.size());
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
  void Controller::recordSunAntialiasing(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (job.sun.empty() || !config.antialiasSun || config.samples < 4 || job.sunAntialiasingFinished) {
      recordReflections(command, config);
      return;
    }
    if (!job.sunAntialiasingPlanned) {
      job.sunAntialiasingPlanned = true;
      const auto owner = m_job;
      job.ticket = m_pipelines->prepare(
          "forward-sun-edges-" + std::to_string(++generation),
          [owner, sites = config.samples >= 16 ? 16u : 4u] {
            const auto begin = std::chrono::steady_clock::now();
            owner->sunAntialiasing = std::make_unique<SunAntialiasing>(
                *owner->source, owner->surfaceLayout, owner->sun, owner->cancellation.get_token(), sites);
            owner->sunAntialiasingCpuMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            if (std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX")) {
              std::fprintf(stderr, "Sunlight edge refinement: %u/%zu receivers, %llu metadata bytes\n",
                           owner->sunAntialiasing->count(), owner->sun.size(),
                           static_cast<unsigned long long>(owner->sunAntialiasing->memoryBytes()));
              const auto points = owner->sunAntialiasing->receivers();
              probes::dumpTransportData(".sun-edge-ids.bin", points.data(), points.size_bytes());
              std::vector<lightmap::Sun> before;
              before.reserve(points.size());
              for (const auto point : points)
                before.push_back(owner->sun[point]);
              probes::dumpTransportData(".sun-edges-before.bin", before.data(),
                                        before.size() * sizeof(before[0]));
            }
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Finding sunlight shadow edges");
      return;
    }
    if (job.sunAntialiasingReceived == job.sunAntialiasing->count()) {
      if (std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX")) {
        std::vector<lightmap::Sun> after;
        after.reserve(job.sunAntialiasing->count());
        for (const auto point : job.sunAntialiasing->receivers()) after.push_back(job.sun[point]);
        probes::dumpTransportData(".sun-edges-after.bin", after.data(), after.size() * sizeof(after[0]));
        std::fprintf(stderr, "Sunlight edge refinement complete: %u receivers, "
                             "cpu_plan_submit_ms=%.3f gpu_ms=%.3f memory_bytes=%llu fallback_receivers=%u\n",
                     job.sunAntialiasing->count(), job.sunAntialiasingCpuMs, job.sunAntialiasingGpuMs,
                     static_cast<unsigned long long>(job.sunAntialiasing->memoryBytes()),
                     job.sunAntialiasing->fallbackCount());
      }
      job.sunAntialiasingFinished = true;
      job.sunAntialiasing.reset();
      recordReflections(command, config);
      return;
    }
    if (job.sunAntialiasingNext < job.sunAntialiasing->count()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job) throw std::logic_error("Sunlight edge slot reused before its fence");
      const uint32_t sitesPerReceiver = job.sunAntialiasing->siteCount();
      const uint32_t samples =
          std::min(config.samples / sitesPerReceiver, 64u / sitesPerReceiver) * sitesPerReceiver;
      const uint32_t count = std::min(
          Budget::maximumPoints / job.sunAntialiasing->siteCount(),
          job.count(Work::Sun, config, job.sunAntialiasing->count() - job.sunAntialiasingNext, samples));
      const auto begin = std::chrono::steady_clock::now();
      const auto sites = job.sunAntialiasing->sites(job.sunAntialiasingNext, count);
      job.transport->recordSurfaceSun(command, m_slot, sites, samples, config.directDiffuse,
                                      job.sunAntialiasing->siteCount());
      job.sunAntialiasingCpuMs +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
      submission.job = m_job;
      submission.sun = submission.sunAntialiasing = true;
      submission.first = job.sunAntialiasingNext;
      submission.count = count;
      job.sunAntialiasingNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Antialiasing sunlight shadow edges (%u/%u texels)",
                  job.sunAntialiasingReceived, job.sunAntialiasing->count());
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
  void Controller::recordVisibility(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (!job.visibilityFinished && job.visibilityReceived == job.mixedLights->tasks.size()) {
      job.visibilityFinished = !mixed::refine(*job.mixedLights, job.surfaceLayout, job.visibility,
          job.cancellation.get_token(), {}, {}, !job.direct.empty() ? mixed::DirectCapture::Deferred
                                                                  : mixed::DirectCapture::Immediate);
      job.visibilityNext = job.visibilityReceived = 0;
    }
    if (job.visibilityFinished) {
      recordDirect(command, config);
      return;
    }
    if (job.visibilityNext < job.mixedLights->tasks.size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Mixed visibility slot reused before its fence");
      const uint32_t count =
          job.count(Work::Visibility, config, uint32_t(job.mixedLights->tasks.size() - job.visibilityNext),
                    config.samples);
      const auto batch = std::span(job.mixedLights->tasks).subspan(job.visibilityNext, count);
      if (job.transport->hasVisibilityReceivers()) {
        job.transport->recordIndexedVisibility(command, m_slot, batch, config.samples);
      } else {
        std::vector<VisibilityPoint> points;
        points.reserve(count);
        for (const auto &task : batch)
          points.push_back({job.surfaceLayout.points[task.point], task.light});
        job.transport->recordVisibility(command, m_slot, points, config.samples, false);
      }
      submission.job = m_job;
      submission.visibility = true;
      submission.first = job.visibilityNext;
      submission.count = count;
      job.visibilityNext += count;
      job.visibilityPoints += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Baking per-light static visibility (%u/%zu samples)",
                  job.visibilityReceived, job.mixedLights->tasks.size());
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
  void Controller::recordDirect(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (job.direct.empty() || job.directFinished) {
      recordOverflowVisibility(command, config);
      return;
    }
    if (!job.directPlanned) {
      job.directPlanned = true;
      const auto owner = m_job;
      job.ticket = m_pipelines->prepare("forward-direct-" + std::to_string(++generation), [owner] {
        owner->directWork.emplace(*owner->mixedLights, owner->surfaceLayout,
                                  owner->cancellation.get_token());
        owner->directWorkBytes = owner->directWork->memoryBytes();
      });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Preparing retained direct lighting");
      return;
    }
    if (job.directReceived == job.directWork->size()) {
      job.directFinished = true;
      job.directWork.reset();
      recordOverflowVisibility(command, config);
      return;
    }
    if (job.directNext < job.directWork->size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job) throw std::logic_error("Direct-light slot reused before its fence");
      const uint32_t count = job.count(Work::Visibility, config,
          job.directWork->size() - job.directNext, config.samples);
      std::vector<VisibilityPoint> points;
      points.reserve(count);
      for (const auto &task : job.directWork->batch(job.directNext, count))
        points.push_back({job.surfaceLayout.points[task.point], task.light});
      job.transport->recordVisibility(command, m_slot, points, config.samples, true);
      submission.job = m_job;
      submission.direct = true;
      submission.first = job.directNext;
      submission.count = count;
      job.directNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Baking retained direct lighting (%u/%u samples)",
                  job.directReceived, job.directWork->size());
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
  void Controller::recordOverflowVisibility(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    // The outer controller waits for the preparation ticket before touching
    // worker outputs. All readbacks of this round finished before dispatch.
    if (job.overflowPreparing) {
      job.overflowPreparing = false;
      if (job.overflowFinished)
        job.overflowAudit.reset();
    }
    if (job.overflowFinished) {
      recordDirectionals(command, config);
      return;
    }
    if (!job.overflowAudit) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(m_physical, &properties);
      const uint64_t texels = uint64_t(properties.limits.maxImageDimension2D) * properties.limits.maxImageDimension2D;
      const uint64_t metadata = 2 + job.mixedLights->occlusion.words().size();
      job.overflowAudit = std::make_unique<mixed::OverflowAudit>(texels > metadata ? texels - metadata : 0);
    }
    if (job.overflowReceived == job.overflowAudit->tasks().size()) {
      job.overflowPreparing = true;
      const auto owner = m_job;
      job.ticket = m_pipelines->prepare("forward-overflow-" + std::to_string(++generation), [owner] {
        const auto begin = std::chrono::steady_clock::now();
        owner->overflowFinished = !owner->overflowAudit->nextRound(
            *owner->mixedLights, owner->surfaceLayout, owner->cancellation.get_token());
        owner->overflowTiming.roundsMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        ++owner->overflowTiming.rounds;
        owner->overflowNext = owner->overflowReceived = 0;
      });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Preparing overflow visibility masks");
      return;
    }
    const auto tasks = job.overflowAudit->tasks();
    if (job.overflowNext < tasks.size()) {
      const auto begin = std::chrono::steady_clock::now();
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Overflow visibility slot reused before its fence");
      const uint32_t count = job.count(Work::Visibility, config,
                                       uint32_t(tasks.size() - job.overflowNext), config.samples);
      // Classification needs visibility only, not another set of directional SH.
      const auto batch = tasks.subspan(job.overflowNext, count);
      if (job.transport->hasVisibilityReceivers()) {
        job.transport->recordIndexedVisibility(command, m_slot, batch, config.samples);
      } else {
        std::vector<VisibilityPoint> points;
        points.reserve(count);
        for (const auto &task : batch)
          points.push_back({job.surfaceLayout.points[task.point], task.light});
        job.transport->recordVisibility(command, m_slot, points, config.samples, false);
      }
      submission.job = m_job;
      submission.overflowVisibility = true;
      submission.first = job.overflowNext;
      submission.count = count;
      job.overflowNext += count;
      job.overflowTiming.submissionMs +=
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
      ++job.overflowTiming.batches;
      job.overflowTiming.points += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Checking overflow light visibility (%llu/%llu surface/light pairs)",
                  static_cast<unsigned long long>(job.overflowAudit->completedWork()),
                  static_cast<unsigned long long>(job.overflowAudit->totalWork()));
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
  void Controller::recordReflections(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (recordSunDepth(command, config)) return;
    if (job.reflectionRecovery && !job.publishedDiffuse) {
      job.publishedDiffuse = prepareVolume(command);
      m_volume = job.publishedDiffuse;
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
      job.ticket =
          m_pipelines->prepare("forward-probe-sites-" + std::to_string(++generation), [owner, config] {
            auto seeds = config.reflectionProbes
                ? probes::placementSeeds(*owner->source, owner->grid, owner->cancellation.get_token())
                : probes::PlacementSeeds{};
            owner->reflections.prepareNext(*owner->source, {.requested = config.reflectionProbes},
                owner->cancellation.get_token(), std::move(seeds));
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Finding reflection capture sites");
      return;
    }
    if (step == CaptureStep::ValidateSites) {
      dispatch();
      char progress[128];
      std::snprintf(progress, sizeof(progress), "Validating reflection capture sites (%u/%zu)",
                    job.reflections.siteReceived, job.reflections.reflectionSites.size());
      status(REMIXAPI_BAKE_TRACING_EXT, progress);
      return;
    }
    if (step == CaptureStep::Fit) {
      const auto owner = m_job;
      job.ticket =
          m_pipelines->prepare("forward-probe-placement-" + std::to_string(++generation), [owner, config] {
            owner->reflections.prepareNext(*owner->source,
                {.requested = config.reflectionProbes, .detailTolerance = config.reflectionDetailTolerance},
                owner->cancellation.get_token());
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Fitting reflection candidates");
      return;
    }
    if (step == CaptureStep::CheckGeometry) {
      dispatch();
      const auto rays = job.reflections.projectionAudit->rays();
      char progress[128];
      std::snprintf(progress, sizeof(progress), "Checking reflection projection geometry (%u/%zu rays)",
                    job.reflections.geometryReceived, rays.size());
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
      job.ticket =
          m_pipelines->prepare("forward-probe-selection-" + std::to_string(++generation), [owner, config, captureCount] {
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
                  std::as_bytes(std::span(owner->mixedLights->lights.directLighting.lights))};
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
      job.ticket = m_pipelines->prepare(
          "forward-probe-filter-" + std::to_string(++generation), [owner, physical, device, config] {
            // Preserve exact input provenance on the worker, then release ray
            // scratch before filtered cubemaps, denoising and atlas encoding.
            // Publication must not scan this potentially large buffer again.
            owner->reflections.complete(owner->cancellation.get_token());
            if (!owner->reflectionRecovery) {
              dumpPhaseFingerprint("transport", owner->source->contentHash, owner->grid, owner->surfaceLayout, owner->surfaces,
                                   owner->sun, owner->reflections.captureView());
              dumpVisibilityReport(*owner->mixedLights, owner->surfaceLayout,
                                    Options{}.bias / owner->source->sceneScale, owner->cancellation.get_token());
            }
            owner->finishStage.store(Job::FinishStage::Reflections, std::memory_order_relaxed);
            owner->reflections.filter(physical, device, owner->cancellation.get_token());
            owner->reflections.retainPayload();
            owner->reflections.sealImage();
            if (!owner->reflectionRecovery)
              owner->finishDiffuseAssets(physical, device, config, owner->reflections.captureView());
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Finishing baked lighting assets");
      return;
    }
    dispatch();
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Capturing reflection cubemaps (%u/%zu texels, %u samples)",
                  job.reflections.captureReceived, job.reflections.work.rays().size(), config.samples);
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
    return;
  }
  void Controller::publishReadyReflections(VkCommandBuffer command) {
    if (!m_job) return;
    const auto image = m_job->reflections.recordImageUpload(command);
    if (!image) return;
    m_reflections = image;
    m_reflectionSceneHash = m_job->source->contentHash;
    m_submissions[m_slot].reflectionUpload = image;
  }
  void Controller::activate(VkCommandBuffer command) {
    if (auto upload = m_job->reflections.recordImageUpload(command))
      m_submissions[m_slot].reflectionUpload = std::move(upload);
    auto reflections = std::move(m_job->reflections.image);
    if (!reflections) throw std::logic_error("Missing prepared reflection image");
    const bool diffusePublished = bool(m_job->publishedDiffuse);
    auto volume = diffusePublished ? m_job->publishedDiffuse : prepareVolume(command);
    volume->reflectionEvidence = m_job->reflectionEvidence;
    volume->localReflections = m_job->localReflections;
    if (volume->localReflections) {
      volume->localReflections->image->recordUpload(command);
      m_submissions[m_slot].localReflectionUpload = volume->localReflections->image;
    }
    probes::dumpCaptures(m_hash, m_job->reflections.captureView());
    if (!m_job->reflections.captureView().empty()) {
      volume->content.captureInputs = m_job->reflections.inputHash;
      uint64_t captured = 1469598103934665603ull;
      for (const auto &capture : m_job->reflections.captureView())
        for (const auto &color : capture.radiance)
          captured = scene::hashBytes(color.data(), 3 * sizeof(float), captured);
      volume->content.captureRadiance = captured | 1;
    }
    const auto specular = probes::serialize(m_job->reflections.filteredView());
    probes::dumpPublishedData(".probes-published.bin", specular.data(), specular.size());
    volume->content.reflections = scene::fastHash(specular.data(), specular.size()) | 1;
    // The transfer and its visibility barrier precede this frame's surface
    // draws. Other frame slots keep their own previous volume alive.
    m_reflectionSceneHash = volume->hash;
    m_reflections = std::move(reflections);
    m_volume = std::move(volume);
    if (!diffusePublished) m_submissions[m_slot].upload = m_volume;
    char message[sizeof(m_statistics.status)];
    const char *label = m_job->cacheHit ? "Cached lighting active"
        : m_job->reflections.cachedRadiance ? "Baked lighting active; reused reflections" : "Baked lighting active";
    if (m_job->surfaceReceived == 0)
      std::snprintf(message, sizeof(message), "%s", label);
    else
      std::snprintf(message, sizeof(message),
                    "%s (%zu surface texels, %u atlas pages; %.3g m surface luxels, %u seam samples, %u curved)",
                    label, size_t(m_job->surfaceReceived), m_job->surfaceLayout.pages, m_job->surfaceLayout.luxelMetres,
                    m_job->surfaceLayout.seamSamples, m_job->surfaceLayout.curvedSeamSamples);
    if (m_job->mixedLights->overflow) {
      const size_t used = std::strlen(message);
      std::snprintf(message + used, sizeof(message) - used, "; %u direct links use overflow fallback",
                    m_job->mixedLights->overflow);
    }
    if (const auto blocked = m_job->mixedLights->occlusion.count()) {
      const size_t used = std::strlen(message);
      std::snprintf(message + used, sizeof(message) - used, "; %u fully blocked links", blocked);
    }
    if (const auto clear = m_job->mixedLights->occlusion.clearCount()) {
      const size_t used = std::strlen(message);
      std::snprintf(message + used, sizeof(message) - used, "; %u fully clear links", clear);
    }
    if (m_job->cacheHit) {
      const size_t used = std::strlen(message);
      std::snprintf(message + used, sizeof(message) - used, "; disk cache");
    }
    if (const char *profile = std::getenv("REMIX_DEBUG_BAKE_TIMING");
        profile && std::strcmp(profile, "1") == 0) {
      std::fprintf(stderr, "Bake CPU readback: total_ms=%.3f bytes=%llu\n", m_job->readbackMs,
                   static_cast<unsigned long long>(m_job->readbackBytes));
      std::fprintf(stderr, "Bake emitter bounds: hemisphere_rejected=%llu cone_rejected=%llu\n",
                   static_cast<unsigned long long>(m_job->mixedLights->hemisphereRejectedPairs),
                   static_cast<unsigned long long>(m_job->mixedLights->coneRejectedPairs));
      std::fprintf(stderr, "Bake retained direct: classified_points=%llu retained_points=%u "
                           "removed_scratch_bytes=%llu task_index_bytes=%llu\n",
                   static_cast<unsigned long long>(m_job->visibilityPoints), m_job->directReceived,
                   static_cast<unsigned long long>(m_job->surfaceLayout.points.size() * sizeof(lightmap::Direct)),
                   static_cast<unsigned long long>(m_job->directWorkBytes));
      const auto &overflow = m_job->overflowTiming;
      std::fprintf(stderr, "Bake overflow timing: points=%llu batches=%llu rounds=%llu "
                           "cpu_rounds_ms=%.3f cpu_submission_ms=%.3f cpu_readback_ms=%.3f gpu_ms=%.3f "
                           "indexed_receivers=%u receiver_bytes=%llu\n",
                   static_cast<unsigned long long>(overflow.points),
                   static_cast<unsigned long long>(overflow.batches),
                   static_cast<unsigned long long>(overflow.rounds), overflow.roundsMs,
                   overflow.submissionMs, overflow.readbackMs, overflow.gpuMs,
                   uint32_t(m_job->transport && m_job->transport->hasVisibilityReceivers()),
                   static_cast<unsigned long long>(m_job->transport ? m_job->transport->visibilityReceiverBytes() : 0));
      constexpr const char *names[] = {"grid",  "surface",     "visibility", "sun",
                                       "sites", "reflections", "geometry", "probe_depth", "coarse_transfer"};
      static_assert(std::size(names) == size_t(Work::Count));
      for (uint32_t i = 0; i < uint32_t(Work::Count); ++i) {
        const auto &timing = m_job->budget.statistics(Work(i));
        std::fprintf(stderr, "Bake GPU timing: %s points=%llu batches=%llu total_ms=%.3f "
                             "longest_ms=%.3f largest_batch=%u\n",
                     names[i], static_cast<unsigned long long>(timing.points),
                     static_cast<unsigned long long>(timing.batches), timing.milliseconds,
                     timing.longest, timing.largest);
      }
    }
    // GPU upload data and runtime bindings now own everything required for
    // publication. Move CPU cache inputs into an independent immutable task.
    // A slow/full/unavailable disk must never delay this ready transition.
    if ((!m_job->cacheHit || m_job->reflectionRecovery) && !m_job->cacheFile.empty()) {
      try {
        if (!cache::persistence().submit(m_job->cacheFile, m_job->cacheKey, m_job->takeCacheSnapshot()))
          std::fprintf(stderr, "Background bake cache budget busy; retaining published lighting without persistence\n");
      } catch (const std::exception &error) {
        std::fprintf(stderr, "Background bake cache unavailable: %s\n", error.what());
      }
    }
    if (m_job->cacheHit && m_job->localReflectionCache && !m_job->cacheFile.empty()) {
      try {
        auto companion = std::make_unique<cache::Snapshot>();
        companion->localReflections = std::move(m_job->localReflectionCache);
        companion->localReflectionsOnly = true;
        if (!cache::persistence().submit(m_job->cacheFile, m_job->cacheKey, std::move(companion)))
          std::fprintf(stderr, "Local reflection companion budget busy; base cache unchanged\n");
      } catch (const std::exception &error) {
        std::fprintf(stderr, "Local reflection companion persistence unavailable: %s\n", error.what());
      }
    }
    m_job.reset();
    status(REMIXAPI_BAKE_READY_EXT, message);
  }
  void Controller::record(VkCommandBuffer command, RasterRenderer &resources, const Config &config,
                          float lightCutoff) {
    std::erase_if(m_obsolete, [](const auto &job) { return job->ticket->ready(); });
    if (!m_emptySurfaceReady) {
      m_emptyReflections->recordUpload(command);
      m_emptyReflectionUpload = m_slot;
      const VkImageMemoryBarrier ready{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = m_emptySurface->handle,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, lightmap::atlasLayers}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &ready);
      m_emptySurfaceReady = true;
      m_emptyOcclusion->clear(command);
    }
    const auto source = resources.bakeScene();
    const uint64_t hash = source && source->ready ? source->contentHash : 0;
    if (source != m_source || hash != m_hash || !config.sameLighting(m_config) ||
        lightCutoff != m_lightCutoff) {
      // A content hash excludes live handles. Even identical lighting needs
      // fresh receiver bindings when its retained scene is replaced. Record
      // that distinction so a late asset or adapter update is diagnosable.
      if (m_job || m_volume)
        std::fprintf(stderr,
            "Forward+ bake invalidated: scene %llu -> %llu, content %llu -> %llu, "
            "bindingsChanged %u, lightingSettingsChanged %u, cutoffChanged %u, published %u\n",
            static_cast<unsigned long long>(m_source ? m_source->sceneId : 0),
            static_cast<unsigned long long>(source ? source->sceneId : 0),
            static_cast<unsigned long long>(m_hash), static_cast<unsigned long long>(hash),
            unsigned(source != m_source), unsigned(!config.sameLighting(m_config)),
            unsigned(lightCutoff != m_lightCutoff), unsigned(bool(m_volume)));
      invalidate();
      m_source = source;
      m_hash = hash;
      m_config = config;
      m_lightCutoff = lightCutoff;
      status(REMIXAPI_BAKE_WAITING_EXT, "Waiting for complete static map inputs");
    }
    m_config = config; // execution controls may change without discarding lighting
    m_statistics.requestedScene = source ? source->sceneId : 0;
    m_statistics.requestedHash = hash;
    m_statistics.samples = config.samples;
    m_statistics.bounces = config.bounces;
    try {
      auto advance = [&] {
        if (!config.enabled) {
          status(REMIXAPI_BAKE_DISABLED_EXT, "Map baking disabled");
          return;
        }
        if (!config.batch || config.batch > 1024 || !std::isfinite(config.gpuBudgetMs) ||
            config.gpuBudgetMs < 0 || config.gpuBudgetMs > 16)
          throw std::invalid_argument("Invalid bake work budget");
        if (!m_statistics.supported) {
          status(REMIXAPI_BAKE_UNSUPPORTED_EXT, "Hardware ray-query baking unavailable");
          return;
        }
        if (!hash) {
          status(REMIXAPI_BAKE_WAITING_EXT, "Waiting for complete static map inputs");
          return;
        }
        if ((config.aggregateDiffuse || config.fullyBakedLights || config.bakedGeometryLights) &&
            !resources.bakeLightsCurrent()) {
          if (m_job || m_volume) {
            std::fprintf(stderr, "Forward+ bake invalidated: captured light dependencies changed, published %u\n",
                         unsigned(bool(m_volume)));
            invalidate();
          }
          status(REMIXAPI_BAKE_WAITING_EXT, "Static lights changed; waiting for updated map inputs");
          return;
        }
        if ((m_volume && !m_job) || m_statistics.state == REMIXAPI_BAKE_FAILED_EXT)
          return;
        if (!m_job) {
          if (!config.samples || config.samples > 8192 || config.probeSamples > 8192 || !config.bounces || config.bounces > 8 ||
              !config.batch || config.batch > 1024 ||
              ((config.styledIndirect || config.groupedStyledDirect) && (!config.styleStorageMiB || config.styleStorageMiB > 512)))
            throw std::invalid_argument("Invalid bake batch settings");
          auto job = std::make_shared<Job>();
          job->source = source;
          const auto physical = m_physical;
          const auto device = m_device;
          const auto queueFamily = m_queueFamily;
          // The worker owns only this frozen source and its private outputs;
          // no camera-frame resources or controller state cross threads.
          auto prepare = [job, physical, device, config, lightCutoff, queueFamily] {
            job->prepareResources(physical, device, config, lightCutoff, queueFamily);
            if (job->cacheHit && !job->reflectionRecovery) {
              job->reflections.image = std::make_shared<probes::ImageAsset>(
                  physical, device, job->reflections.filteredView(), job->cancellation.get_token());
              job->restoreLocalReflections(physical, device, config);
              job->prepareLocalReflectionUpgrade(physical, device, config, queueFamily);
              job->reflections.retainPayload();
              job->reflections.sealImage();
            }
          };
          job->ticket =
              m_pipelines->prepare("forward-bake-" + std::to_string(++generation), std::move(prepare));
          m_job = std::move(job);
          status(REMIXAPI_BAKE_PREPARING_EXT, "Preparing static bake resources");
        }
        publishReadyReflections(command);
        if (!m_job->ticket->ready()) {
          const auto total = m_job->denoiseChartCount.load(std::memory_order_relaxed);
          const auto stage = m_job->finishStage.load(std::memory_order_relaxed);
          if (stage == Job::FinishStage::Denoising && total) {
            char progress[96];
            std::snprintf(progress, sizeof(progress), "Denoising surface lightmaps (%u/%u charts)",
                          m_job->denoisedCharts.load(std::memory_order_relaxed), total);
            status(REMIXAPI_BAKE_PREPARING_EXT, progress);
          } else {
            const char *message = nullptr;
            switch (stage) {
            case Job::FinishStage::ProbeRefinement: message = "Refining irradiance probe placement"; break;
            case Job::FinishStage::CoarseTransfer: message = "Preparing coarse probe connection visibility"; break;
            case Job::FinishStage::Reflections: message = "Filtering reflection cubemaps"; break;
            case Job::FinishStage::Denoising: message = "Denoising surface lightmaps"; break;
            case Job::FinishStage::Atlas: message = "Encoding surface lightmaps"; break;
            case Job::FinishStage::Geometry: message = "Preparing lightmapped geometry"; break;
            case Job::FinishStage::Styles: message = "Encoding styled indirect lightmaps"; break;
            case Job::FinishStage::None: break;
            }
            if (message)
              status(REMIXAPI_BAKE_PREPARING_EXT, message);
          }
          return;
        }
        m_job->ticket->check();
        m_job->initialized = true;
        if (m_job->failed)
          return;
        auto &job = *m_job;
        m_statistics.probes = uint32_t(job.grid.tasks.size());
        m_statistics.completed = job.received;
        if (job.reflectionRecovery && !job.filtering) {
          if (!job.prepared) {
            auto &submission = m_submissions[m_slot];
            if (submission.job) throw std::logic_error("Reflection recovery reused a live slot");
            job.transport->recordPrepare(command, resources);
            if (job.sunDepth && job.sunDepth->restored()) job.sunDepth->recordUpload(command);
            job.prepared = true;
            submission.job = m_job;
            submission.preparation = true;
            status(REMIXAPI_BAKE_PREPARING_EXT, "Preparing reflections from cached diffuse lighting");
          } else {
            recordReflections(command, config);
          }
          return;
        }
        if (job.filtering) {
          if ((job.sunDepth && !job.sunDepth->restored()) || job.localReflectionUpgrade) {
            if (!job.prepared) {
              auto &submission = m_submissions[m_slot];
              if (submission.job) throw std::logic_error("Cached sun depth preparation reused a live slot");
              job.transport->recordPrepare(command, resources);
              job.prepared = true;
              submission.job = m_job;
              submission.preparation = true;
              status(REMIXAPI_BAKE_PREPARING_EXT, "Preparing missing auxiliary data from cached lighting");
              return;
            }
            if (recordSunDepth(command, config)) return;
          }
          if (job.localReflectionUpgrade && recordReceiverDiagnostics(command, config)) return;
          if (recordLocalReflections(command, config)) return;
          activate(command);
          return;
        }
        if (job.baseFinished && job.styles) {
          recordStyles(command, config);
          return;
        }
        if (!job.prepared) {
          auto &submission = m_submissions[m_slot];
          job.transport->recordPrepare(command, resources);
          if (job.sunDepth && job.sunDepth->restored()) job.sunDepth->recordUpload(command);
          job.prepared = true;
          submission.job = m_job;
          submission.preparation = true;
          status(REMIXAPI_BAKE_TRACING_EXT, "Preparing static transport on GPU");
          return;
        }
        if (job.reference && job.reference->received < job.reference->tasks()) {
          auto &reference = *job.reference;
          if (reference.next < reference.tasks()) {
            auto &submission = m_submissions[m_slot];
            if (submission.job) throw std::logic_error("Reference slot reused before its fence");
            const auto count = std::min(reference.geometry ? 16u : reference.usesRadianceRays() ? 256u : 1u, reference.tasks() - reference.next);
            const auto referencePoints = reference.batch(reference.next, count);
            const uint32_t firstSample = reference.sampleOffsets.empty() || reference.usesRadianceRays()
                ? 0 : reference.sampleOffsets.at(reference.next);
            if (reference.geometry)
              job.transport->recordGeometry(command, m_slot, referencePoints);
            else if (reference.usesRadianceRays() && reference.combinedLighting)
              job.transport->recordRadianceComponent(command, m_slot, referencePoints, Transport::combinedLighting,
                                                     config.bounces);
            else if (reference.usesRadianceRays())
              job.transport->recordRadiance(command, m_slot, referencePoints, config.bounces);
            else if (reference.incomingIrradiance)
              job.transport->recordIncomingIrradiance(command, m_slot, referencePoints, reference.samples,
                  config.bounces, firstSample, reference.combinedLighting
                      ? std::optional<uint32_t>(Transport::combinedLighting) : std::nullopt);
            else if (reference.combinedLighting)
              job.transport->recordComponent(command, m_slot, referencePoints, Transport::combinedLighting,
                                             reference.samples, config.bounces, firstSample);
            else
              job.transport->recordBatch(command, m_slot, referencePoints, reference.samples, config.bounces,
                                         firstSample);
            submission.job = m_job;
            submission.reference = true;
            submission.first = reference.next;
            submission.count = count;
            reference.next += count;
          }
          status(REMIXAPI_BAKE_TRACING_EXT, job.reference->geometry ? "Tracing offline geometry reference"
                                                               : "Tracing offline irradiance reference");
          return;
        }
        if (job.received == job.grid.tasks.size()) {
          if (job.refinement.factor && !job.refinementPlanned) {
            // Reuse the required lightmap bake as a local irradiance reference.
            // No extra rays are needed to locate interpolation errors.
            if (job.surfaceReceived < job.surfaceLayout.points.size()) {
              recordSurfaces(command, config);
              return;
            }
            job.refinementPlanned = true;
            job.finishStage.store(Job::FinishStage::ProbeRefinement, std::memory_order_relaxed);
            auto owner = m_job;
            job.ticket = m_pipelines->prepare("forward-probe-refinement-" + std::to_string(++generation),
                                             [owner, config] { owner->refineProbes(config); });
            status(REMIXAPI_BAKE_PREPARING_EXT, "Refining irradiance probe placement");
            return;
          }
          if (!job.finalProbePass && config.probeSampleCount() != config.samples) {
            // Keep the adaptive allocator's reference budget unchanged. Only
            // after placement is fixed may probe quality change its estimates.
            job.finalProbePass = true;
            job.next = job.received = 0;
            status(REMIXAPI_BAKE_TRACING_EXT, "Improving final irradiance probe quality");
            return;
          }
          if (recordCoarseTransfer(command, config)) return;
          if (config.directionalProbeDepth && job.probeDepthReceived < job.grid.tasks.size())
            recordProbeDepth(command, config);
          else recordProbeSun(command, config);
          return;
        }
        auto &submission = m_submissions[m_slot];
        if (submission.job)
          throw std::logic_error("Bake slot reused before its fence");
        if (job.next < job.grid.tasks.size()) {
          const auto samples = job.finalProbePass ? config.probeSampleCount() : config.samples;
          const uint32_t count =
              job.count(Work::Irradiance, config, uint32_t(job.grid.tasks.size() - job.next), samples,
                        config.bounces);
          std::vector<Point> points;
          points.reserve(count);
          for (uint32_t i = 0; i < count; ++i)
            points.push_back(job.grid.points[job.grid.tasks[job.next + i]]);
          job.transport->recordBatch(command, m_slot, points, samples, config.bounces);
          submission.job = m_job;
          submission.first = job.next;
          submission.count = count;
          job.next += count;
        }
        status(REMIXAPI_BAKE_TRACING_EXT, "Baking static diffuse irradiance");
      };
      advance();
    } catch (VkResult error) {
      if (error == VK_ERROR_DEVICE_LOST)
        throw;
      if (m_job)
        m_job->failed = true;
      char message[96];
      std::snprintf(message, sizeof(message), "Bake unavailable (Vulkan %d)", int(error));
      status(REMIXAPI_BAKE_FAILED_EXT, message);
    } catch (const std::exception &error) {
      if (m_job)
        m_job->failed = true;
      // The public status text is bounded; retain full diagnostic provenance.
      std::fprintf(stderr, "Forward+ bake failed: %s\n", error.what());
      status(REMIXAPI_BAKE_FAILED_EXT, error.what());
    }
    m_statistics.active = bool(m_volume);
    m_statistics.activeHash = m_volume ? m_volume->hash : 0;
    m_submissions[m_slot].sampled = m_volume;
    m_submissions[m_slot].reflectionSampled = m_reflections;
    const auto used = memory();
    m_statistics.memoryBytes = used.buffers + used.images + used.acceleration;
  }
  VkDescriptorBufferInfo Controller::descriptor() const {
    return m_volume ? m_volume->data->descriptor() : m_empty->descriptor();
  }
  VkDescriptorBufferInfo Controller::sunDepthTable() const {
    return m_volume && m_volume->sunDepth ? m_volume->sunDepth->table() : m_emptySunDepth->descriptor();
  }
  VkDescriptorBufferInfo Controller::sunDepthValues() const {
    return m_volume && m_volume->sunDepth ? m_volume->sunDepth->values() : m_emptySunDepth->descriptor();
  }
  probes::Descriptors Controller::reflectionDescriptor() const {
    return m_reflections ? m_reflections->descriptors() : m_emptyReflections->descriptors();
  }
  VkDescriptorImageInfo Controller::surfaceDescriptor() const {
    const auto &image = m_volume && m_volume->surfaceAtlas ? m_volume->surfaceAtlas : m_emptySurface;
    return {VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
  VkDescriptorImageInfo Controller::occlusionDescriptor() const {
    const auto &image = m_volume ? m_volume->occlusion : m_emptyOcclusion;
    return image->descriptor();
  }
  VkDescriptorImageInfo Controller::visibilityDescriptor() const {
    if (m_volume && m_volume->visibilityPages)
      return m_volume->visibilityPages->descriptor();
    // An unused floating-point array descriptor can share the existing empty
    // surface view. Do not reserve another image for maps with no R8 pages.
    return {VK_NULL_HANDLE, m_emptySurface->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
  Memory Controller::memory() const {
    Memory result;
    result.buffers = m_emptySunDepth->allocationBytes + m_empty->allocationBytes + m_emptyReflections->bufferBytes();
    result.images = m_emptySurface->bytes + m_emptyOcclusion->bytes() + m_emptyReflections->imageBytes();
    std::unordered_set<const lightmap::Geometry *> surfaces;
    auto addSurfaces = [&](const std::shared_ptr<lightmap::Geometry> &geometry) {
      if (geometry && surfaces.insert(geometry.get()).second)
        result.buffers += geometry->memoryBytes();
    };
    std::unordered_set<const SunDepth *> depths;
    auto addDepth = [&](const std::shared_ptr<SunDepth> &depth) {
      if (depth && depths.insert(depth.get()).second) result.buffers += depth->bytes();
    };
    std::unordered_set<const probes::ImageAsset *> localImages;
    auto addReflection = [&](const std::shared_ptr<probes::ImageAsset> &image) {
      if (image && localImages.insert(image.get()).second) {
        result.buffers += image->bufferBytes();
        result.images += image->imageBytes();
      }
    };
    auto addLocalAsset = [&](const std::shared_ptr<probes::ImageAsset> &image) {
      if (image && localImages.insert(image.get()).second) {
        result.buffers += image->bufferBytes();
        result.images += image->imageBytes();
      }
    };
    auto addLocalImage = [&](const std::shared_ptr<const LocalReflections> &local) {
      if (local) addLocalAsset(local->image);
    };
    std::unordered_set<const Job *> jobs;
    auto addJob = [&](const std::shared_ptr<Job> &job) {
      if (!job || !job->initialized || !jobs.insert(job.get()).second)
        return;
      // Private preparation outputs may still be written on the worker. The
      // independently published reflection owner is counted separately below.
      if (job->ticket->ready()) {
        addSurfaces(job->surfaceGeometry);
        addDepth(job->sunDepth);
        for (const auto &slot : job->coarseTransferSlots) {
          if (slot.output) result.buffers += slot.output->allocationBytes;
          if (slot.readback) result.buffers += slot.readback->allocationBytes;
        }
        addReflection(job->reflections.image);
        addLocalImage(job->localReflections);
      }
      if (!job->transport)
        return; // a disk hit needs only geometry and the published volume upload
      const auto &transport = *job->transport;
      result.acceleration += transport.accelerationBytes();
      result.images += transport.imageBytes();
      result.buffers += transport.memoryBytes() - transport.accelerationBytes() - transport.imageBytes();
    };
    addJob(m_job);
    for (const auto &submission : m_submissions)
      addJob(submission.job);
    std::unordered_set<const Volume *> volumes;
    auto addVolume = [&](const std::shared_ptr<Volume> &volume) {
      if (!volume || !volumes.insert(volume.get()).second)
        return;
      addSurfaces(volume->surfaces);
      addDepth(volume->sunDepth);
      if (volume->surfaceAtlas)
        result.images += volume->surfaceAtlas->bytes;
      if (volume->occlusion)
        result.images += volume->occlusion->bytes();
      if (volume->visibilityPages)
        result.images += volume->visibilityPages->bytes();
      addLocalImage(volume->localReflections);
      result.buffers += volume->data->allocationBytes;
      if (volume->staging)
        result.buffers += volume->staging->allocationBytes;
    };
    addVolume(m_volume);
    addReflection(m_reflections);
    for (const auto &submission : m_submissions) {
      addVolume(submission.upload);
      addVolume(submission.sampled);
      addReflection(submission.reflectionUpload);
      addReflection(submission.reflectionSampled);
      addLocalAsset(submission.localReflectionUpload);
    }
    return result;
  }
} // namespace remix::forward::bake
