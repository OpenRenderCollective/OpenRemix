#include "forward/bake/job.h"
#include "forward/bake/model_charts.h"
#include "forward/probes/priority_bands.h"
#include "platform/workers.h"
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
#include "forward/lightmap/denoise.h"
#include "forward/lightmap/interior_donors.h"
#include "forward/mixed/overflow.h"
#include "forward/mixed/ownership.h"
#include "forward/mixed/mask_pages.h"
#include "forward/mixed/mask_lookup.h"
#include "forward/mixed/publication.h"
#include "forward/lightmap/upload.h"
#include "forward/probes/capture_dump.h"
#include "forward/probes/data.h"
#include "forward/probes/evaluation.h"
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
    constexpr uint32_t reflectionSiteSamples = 64;
    template <typename T> void releaseStorage(std::vector<T> &values) {
      std::vector<T>().swap(values);
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
  void Controller::Job::finishCaptureInputs() {
    if (!captures.empty())
      captureInputHash = scene::fastHash(captureWork.rays().data(), captureWork.rays().size() * sizeof(Point)) | 1;
    captureWork = {};
  }
  void Controller::Job::prepareResources(VkPhysicalDevice physical, VkDevice device, const Config &config,
                                         float lightCutoff, uint32_t queueFamily) {
    // Initial preparation owns this job exclusively until its ticket completes.
    // Keep caller identity/bindings intact and derive only private atlas inputs.
    if (modelChartsRequested()) {
      if (!config.lightmapFitBudget)
        throw std::invalid_argument("Model charts require world-metric budget fitting");
      auto charts = modelCharts(*source, 65536, cancellation.get_token());
      auto derived = std::make_shared<scene::BakeScene>(*source);
      derived->charts.insert(derived->charts.end(), charts.begin(), charts.end());
      std::fprintf(stderr, "Forward+ model charts: authored %zu, generated %zu (candidate)\n",
                   source->charts.size(), charts.size());
      source = std::move(derived);
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    const auto directLighting = (config.aggregateDiffuse || config.fullyBakedLights || config.bakedGeometryLights)
        ? staticAggregatePolicy(*source, config.fullyBakedLights ? DirectMode::Baked : DirectMode::BakedMixed,
                                 config.bakedGeometryLights)
        : DirectPolicy{};
    const char *samples = std::getenv("REMIX_DEBUG_BAKE_SURFACE_SAMPLES");
    reference = ReferenceCapture::fromEnvironment();
    const bool depthCapture = std::getenv("REMIX_DEBUG_BAKE_SUN_DEPTH") != nullptr;
    if (!reference && config.diskCache && (!samples || std::strcmp(samples, "1") != 0)) {
      cache::Device deviceIdentity{properties.vendorID, properties.deviceID, properties.driverVersion,
                                     properties.limits.maxImageDimension2D,
                                     properties.limits.maxImageArrayLayers,
                                     properties.limits.maxStorageBufferRange,
                                     {}};
      std::copy_n(properties.pipelineCacheUUID, deviceIdentity.uuid.size(), deviceIdentity.uuid.begin());
      cacheKey = cache::key(source->contentHash, config, lightCutoff, deviceIdentity, directLighting);
      try {
        cacheFile = cache::path(cacheKey);
      } catch (const std::exception &) {
        cacheFile.clear();
      }
      if (!cacheFile.empty()) {
        auto cached = cache::read(cacheFile, cacheKey, source, config, deviceIdentity,
                                  cancellation.get_token(), directLighting);
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
                               cache::key(0, config, lightCutoff, deviceIdentity, directLighting), bool(cached));
        if (cached) {
          grid = std::move(cached->grid);
          surfaceLayout = std::move(cached->layout);
          mixedLights = std::make_shared<mixed::Plan>(std::move(cached->mixed));
          captures = std::move(cached->captures);
          specular = std::move(cached->specular);
          // Influence and edge bands are derived metadata, not baked lighting.
          // Apply current overlap to retained captures without invalidating a
          // hit.
          probes::configureInfluence(*source, captures,
                                     config.reflectionDetailTolerance,
                                     cancellation.get_token());
          for (size_t i = 0; i < captures.size(); ++i)
            specular.header.probes[i] = probes::projectionRecord(captures[i]);
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
          captureInputHash = probes::CaptureWork::inputHash(captures, cancellation.get_token());
          if (!source->charts.empty())
            surfaceGeometry = std::make_shared<lightmap::Geometry>(
                physical, device, source, surfaceLayout, cancellation.get_token(),
                mixedLights->charts, mixedLights->directCharts);
          dumpReceiverReport(*source, surfaceLayout, surfacePixels, grid, cancellation.get_token());
          prepareSurfaceStorage();
          prepareGroupedStyles(properties.limits.maxStorageBufferRange, config);
          if (sunDepth && !sunDepth->restored()) {
            // A missing depth sidecar must not retrace cached irradiance/styles.
            Options depthOptions;
            depthOptions.depthCapture = true;
            depthOptions.batchPoints = Budget::maximumPoints;
            depthOptions.rayBudget = Budget::maximumWork;
            depthOptions.initialBatchPoints = 1024;
            depthOptions.queueFamily = queueFamily;
            depthOptions.textureResolution = config.textureResolution;
            transport = std::make_unique<Transport>();
            transport->initialize(physical, device, source, depthOptions);
          }
          cacheHit = filtering = true;
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
    lightmap::Options atlas;
    // Vertex UVs address texel centres and carry explicit chart clamp bounds.
    // The outward-rounded chart grid already contains its bilinear footprint.
    atlas.padding = 0;
    atlas.pageSize = config.lightmapPageSizeForBudget();
    atlas.luxelMetres = config.lightmapLuxelSize;
    atlas.fillBudget = config.lightmapFitBudget;
    atlas.maxTexels = config.lightmapTexels;
    atlas.maxPages = std::min(256u, properties.limits.maxImageArrayLayers / lightmap::atlasLayers);
    if (atlas.pageSize > properties.limits.maxImageDimension2D)
      throw std::invalid_argument("Surface lightmap page exceeds device limit");
    surfaceLayout = lightmap::fit(*source, atlas, cancellation.get_token());
    grid = placeRefinementBase(*source, placement, surfaceLayout, refinement, cancellation.get_token());
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
    options.depthCapture = bool(sunDepth);
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
      reference->bounces = config.bounces;
      reference->combinedLighting = options.lightingComponents;
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
      submission.upload->specular->releaseUpload();
      if (submission.upload->localReflections) submission.upload->localReflections->image->releaseUpload();
    }
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
          } else if (submission.reflectionGeometry) {
            if (!job.projectionAudit->observe(submission.first, results)) {
              job.failed = true;
              status(REMIXAPI_BAKE_FAILED_EXT, "Invalid reflection projection geometry");
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
              if (submission.radiance) {
                const uint32_t ray = submission.first + i;
                const auto [capture, texel] = job.captureWork.destination(ray);
                auto color = value.irradiance[0];
                for (uint32_t c = 0; c < 3; ++c)
                  color[c] = std::clamp(color[c], 0.f, 65000.f);
                job.captures[capture].radiance[texel] = color;
              } else if (submission.reflectionSite) {
                job.usableReflectionSites[submission.first + i] =
                    !job.failed && value.statistics[2] == reflectionSiteSamples &&
                    std::isfinite(value.statistics[0]) && value.statistics[0] >= 0 &&
                    value.statistics[0] < .5f;
              } else if (submission.surface) {
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
        if (submission.directionalField) job.directionalTiming.readbackMs += readbackMs;
        if (submission.overflowVisibility)
          job.overflowTiming.readbackMs += readbackMs;
        job.readbackBytes += uint64_t(submission.count) *
                             (submission.sunDepth ? 0 : submission.overflowVisibility || submission.visibility ||
                              (submission.probeLocal && !job.localProbes->directional()) ? sizeof(float) :
                              submission.direct || submission.directionalField || submission.sun ||
                              submission.probeSun || submission.probeLocal
                                  ? sizeof(VisibilityResult) : sizeof(Result));
        if (!submission.styled && !submission.reference && !submission.probeDepth) (submission.sunDepth ? job.sunDepthReceived
         : submission.localReflection ? job.localCaptureReceived
         : submission.radiance ? job.captureReceived
         : submission.receiverFitCoverage ? job.receiverCoverageReceived
         : submission.receiverFit ? job.receiverFitReceived
         : submission.receiverSiteCheck ? job.receiverCheckReceived
         : submission.receiverSite ? job.receiverSiteReceived
         : submission.reflectionReceiver ? job.receiverReceived
         : submission.reflectionGeometry ? job.geometryReceived
         : submission.reflectionSite     ? job.siteReceived
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
          if (submission.directionalField) job.directionalTiming.gpuMs += *elapsed;
          if (submission.sunAntialiasing) job.sunAntialiasingGpuMs += *elapsed;
          if (submission.overflowVisibility)
            job.overflowTiming.gpuMs += *elapsed;
          const auto work = submission.probeDepth                                    ? Work::ProbeDepth
                            : submission.reflectionGeometry || submission.reflectionReceiver || submission.receiverSiteCheck || submission.receiverFit || submission.receiverFitCoverage || submission.sunDepth ? Work::Geometry
                            : submission.visibility || submission.direct || submission.directionalField ||
                              submission.overflowVisibility || submission.probeLocal     ? Work::Visibility
                            : submission.sun || submission.probeSun                  ? Work::Sun
                            : submission.reflectionSite || submission.receiverSite  ? Work::Site
                            : submission.radiance || submission.localReflection      ? Work::Reflection
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
    if (!job.sitesPlanned) {
      job.sitesPlanned = true;
      const auto owner = m_job;
      job.ticket =
          m_pipelines->prepare("forward-probe-sites-" + std::to_string(++generation), [owner, config] {
            const auto sites = probes::candidateSites(*owner->source, owner->grid, config.reflectionProbes,
                                                      owner->cancellation.get_token());
            owner->reflectionSites.reserve(sites.size());
            for (const auto &position : sites)
              owner->reflectionSites.push_back({{position[0], position[1], position[2], 0}, {}});
            owner->usableReflectionSites.resize(sites.size());
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Finding reflection capture sites");
      return;
    }
    if (job.siteReceived < job.reflectionSites.size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Reflection validation slot reused before its fence");
      if (job.siteNext < job.reflectionSites.size()) {
        const uint32_t count = job.count(
            Work::Site, config, uint32_t(job.reflectionSites.size() - job.siteNext), reflectionSiteSamples);
        job.transport->recordSiteValidation(command, m_slot,
                                            std::span(job.reflectionSites).subspan(job.siteNext, count),
                                            reflectionSiteSamples);
        submission.job = m_job;
        submission.reflectionSite = true;
        submission.first = job.siteNext;
        submission.count = count;
        job.siteNext += count;
      }
      char progress[128];
      std::snprintf(progress, sizeof(progress), "Validating reflection capture sites (%u/%zu)",
                    job.siteReceived, job.reflectionSites.size());
      status(REMIXAPI_BAKE_TRACING_EXT, progress);
      return;
    }
    if (!job.candidatesPlanned) {
      job.candidatesPlanned = true;
      const auto owner = m_job;
      job.ticket =
          m_pipelines->prepare("forward-probe-placement-" + std::to_string(++generation), [owner, config] {
            std::vector<Vec3> validated;
            for (size_t i = 0; i < owner->reflectionSites.size(); ++i)
              if (owner->usableReflectionSites[i]) {
                const auto &position = owner->reflectionSites[i].position;
                validated.push_back({position[0], position[1], position[2]});
              }
            auto regions = probes::fitCandidates(*owner->source, owner->grid, config.reflectionProbes,
                                                 owner->cancellation.get_token(),
                                                 config.reflectionDetailTolerance, &validated);
            if (owner->source->partition && owner->source->partition->closed && !regions.empty())
              owner->projectionAudit = std::make_unique<probes::ProjectionAudit>(
                  std::move(regions), owner->cancellation.get_token());
            else
              owner->candidateRegions = std::move(regions);
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Fitting reflection candidates");
      return;
    }
    if (job.projectionAudit && job.geometryReceived < job.projectionAudit->rays().size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Reflection geometry slot reused before its fence");
      const auto rays = job.projectionAudit->rays();
      if (job.geometryNext < rays.size()) {
        const uint32_t count = job.count(Work::Geometry, config, uint32_t(rays.size() - job.geometryNext), 1);
        job.transport->recordGeometry(command, m_slot, rays.subspan(job.geometryNext, count));
        submission.job = m_job;
        submission.reflectionGeometry = true;
        submission.first = job.geometryNext;
        submission.count = count;
        job.geometryNext += count;
      }
      char progress[128];
      std::snprintf(progress, sizeof(progress), "Checking reflection projection geometry (%u/%zu rays)",
                    job.geometryReceived, rays.size());
      status(REMIXAPI_BAKE_TRACING_EXT, progress);
      return;
    }
    if (!job.capturePlanned) {
      job.capturePlanned = true;
      const auto owner = m_job;
      VkPhysicalDeviceProperties limits;
      vkGetPhysicalDeviceProperties(m_physical, &limits);
      const uint32_t captureCount = std::min(config.reflectionProbes,
          probes::imageCapacity(config.reflectionResolution, limits.limits.maxImageDimension2D,
                                limits.limits.maxImageArrayLayers, limits.limits.maxStorageBufferRange));
      job.ticket =
          m_pipelines->prepare("forward-probe-selection-" + std::to_string(++generation), [owner, config, captureCount] {
            if (owner->projectionAudit) {
              owner->candidateRegions = owner->projectionAudit->finish();
              owner->projectionAudit.reset();
            }
            owner->captures = probes::captureCandidates(
                *owner->source, owner->candidateRegions, captureCount, config.reflectionResolution,
                owner->cancellation.get_token(), config.reflectionDetailTolerance);
            probes::dumpCandidates(*owner->source, owner->candidateRegions, owner->captures,
                                   owner->cancellation.get_token());
            owner->receiverAudit = probes::prepareReceiverAudit(*owner->source, owner->captures,
                                                               owner->cancellation.get_token());
            releaseStorage(owner->candidateRegions);
            owner->captureWork = probes::CaptureWork(owner->captures, owner->cancellation.get_token());
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Selecting reflection captures");
      return;
    }
    if (recordReceiverDiagnostics(command, config)) return;
    if (job.captureReceived == job.captureWork.rays().size()) {
      job.filtering = true;
      const auto owner = m_job;
      const auto physical = m_physical;
      const auto device = m_device;
      job.ticket = m_pipelines->prepare(
          "forward-probe-filter-" + std::to_string(++generation), [owner, physical, device, config] {
            owner->captureWork.complete(owner->captures, owner->cancellation.get_token());
            // Preserve exact input provenance on the worker, then release ray
            // scratch before filtered cubemaps, denoising and atlas encoding.
            // Publication must not scan this potentially large buffer again.
            owner->finishCaptureInputs();
            dumpPhaseFingerprint("transport", owner->source->contentHash, owner->grid, owner->surfaceLayout, owner->surfaces,
                                 owner->sun, owner->captures);
            dumpVisibilityReport(*owner->mixedLights, owner->surfaceLayout,
                                  Options{}.bias / owner->source->sceneScale, owner->cancellation.get_token());
            owner->finishStage.store(Job::FinishStage::Reflections, std::memory_order_relaxed);
            {
              WorkerPool workers;
              owner->specular = probes::filter(owner->captures, 128, owner->cancellation.get_token(), &workers);
            }
            owner->reflectionImage = std::make_unique<probes::ImageAsset>(
                physical, device, owner->specular, owner->cancellation.get_token());
            if (config.denoiseLightmaps && lightmap::denoiserVersion()) {
              owner->finishStage.store(Job::FinishStage::Denoising, std::memory_order_relaxed);
              auto progress = [owner](uint32_t completed, uint32_t total) {
                owner->denoisedCharts.store(completed, std::memory_order_relaxed);
                owner->denoiseChartCount.store(total, std::memory_order_relaxed);
              };
              const auto filtered = lightmap::denoise(owner->surfaceLayout, owner->surfaces,
                                                       owner->cancellation.get_token(), std::move(progress));
              std::fprintf(stderr, "Forward lightmap OIDN: %u charts, %llu texels, device type %d, %.3f s\n",
                           filtered.charts, static_cast<unsigned long long>(filtered.texels),
                           filtered.deviceType, filtered.seconds);
              std::fprintf(stderr, "Forward lightmap OIDN work: %u executions, %u preparations, "
                                   "%u constant fields, %u monitored, %u batches, %u waits; %llu host bytes, %llu image bytes, "
                                   "%llu transfer bytes\n",
                           filtered.executions, filtered.preparations, filtered.constantFields,
                           filtered.monitoredExecutions,
                           filtered.batches, filtered.synchronizations,
                           static_cast<unsigned long long>(filtered.peakHostBytes),
                           static_cast<unsigned long long>(filtered.imageBufferBytes),
                           static_cast<unsigned long long>(filtered.transferBytes));
              std::fprintf(stderr, "Forward lightmap OIDN graphs: %u captures, %u launches, %u fallbacks\n",
                           filtered.graphCaptures, filtered.graphLaunches, filtered.graphFallbacks);
              std::fprintf(stderr, "Forward lightmap OIDN CPU schedule: %u queues, %u threads per queue\n",
                           filtered.cpuQueues, filtered.cpuThreadsPerQueue);
              std::fprintf(stderr, "Forward lightmap OIDN summed worker time: initialize %.3f s, prepare %.3f s, "
                                   "upload %.3f s, execute %.3f s, readback %.3f s, wait %.3f s, chart %.3f s\n",
                           filtered.initializationSeconds, filtered.preparationSeconds,
                           filtered.uploadSeconds, filtered.executionSeconds, filtered.readbackSeconds,
                           filtered.waitSeconds, filtered.chartSeconds);
              std::fprintf(stderr, "Forward lightmap OIDN staging: %llu pinned bytes, %.3f s host setup/copies; "
                                   "%llu GPU batch bytes\n",
                           static_cast<unsigned long long>(filtered.stagingBufferBytes), filtered.stagingSeconds,
                           static_cast<unsigned long long>(filtered.batchBufferBytes));
            }
            dumpPhaseFingerprint("filtered", owner->source->contentHash, owner->grid, owner->surfaceLayout, owner->surfaces,
                                 owner->sun, owner->captures);
            owner->finishStage.store(Job::FinishStage::Atlas, std::memory_order_relaxed);
            owner->surfacePixels =
                lightmap::pixels(owner->surfaceLayout, owner->surfaces, owner->cancellation.get_token(),
                                 owner->visibility, owner->direct, owner->sun);
            dumpReceiverReport(*owner->source, owner->surfaceLayout, owner->surfacePixels, owner->grid,
                                owner->cancellation.get_token());
            // These transport arrays are no longer needed once encoded. Avoid
            // retaining them beside the much larger atlas and upload allocation.
            releaseStorage(owner->visibility);
            releaseStorage(owner->direct);
            releaseStorage(owner->sun);
            const char *samples = std::getenv("REMIX_DEBUG_BAKE_SURFACE_SAMPLES");
            if (config.styledIndirect && !styleIds(*owner->source).empty()) {
              VkPhysicalDeviceProperties limits;
              vkGetPhysicalDeviceProperties(physical, &limits);
              const uint64_t baseBytes = gridBytes(owner->grid);
              const uint64_t localBytes = owner->localProbes ? owner->localProbes->gpuBytes() : 0;
              if (baseBytes + localBytes + 16 >= limits.limits.maxStorageBufferRange)
                throw std::length_error("No device storage range for attributed lighting");
              const uint64_t maximum = std::min(uint64_t(config.styleStorageMiB) << 20,
                  uint64_t(limits.limits.maxStorageBufferRange) - baseBytes - localBytes - 15);
              owner->styles = std::make_unique<StyleWork>(*owner->source, owner->surfaceLayout, owner->grid, maximum);
              owner->styles->takeSurfaces(owner->surfaces, owner->surfaceLayout.points.size());
            } else if (!samples || std::strcmp(samples, "1") != 0) {
              releaseStorage(owner->surfaces);
            }
            owner->finishStage.store(Job::FinishStage::Geometry, std::memory_order_relaxed);
            if (!owner->source->charts.empty())
              owner->surfaceGeometry = std::make_shared<lightmap::Geometry>(
                  physical, device, owner->source, owner->surfaceLayout, owner->cancellation.get_token(),
                  owner->mixedLights->charts, owner->mixedLights->directCharts);
            owner->prepareSurfaceStorage();
            VkPhysicalDeviceProperties groupLimits;
            vkGetPhysicalDeviceProperties(physical, &groupLimits);
            if (!owner->styles) owner->prepareGroupedStyles(groupLimits.limits.maxStorageBufferRange, config);
            owner->baseFinished = true;
            owner->filtering = !owner->styles;
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Finishing baked lighting assets");
      return;
    }
    if (job.captureNext < job.captureWork.rays().size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Reflection slot reused before its fence");
      const uint32_t count = job.count(Work::Reflection, config,
                                       uint32_t(job.captureWork.rays().size() - job.captureNext),
                                       config.samples, config.bounces);
      job.transport->recordRadiance(
          command, m_slot, job.captureWork.rays().subspan(job.captureNext, count), config.bounces, config.samples);
      submission.job = m_job;
      submission.radiance = true;
      submission.first = job.captureNext;
      submission.count = count;
      job.captureNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Capturing reflection cubemaps (%u/%zu texels, %u samples)",
                  job.captureReceived, job.captureWork.rays().size(), config.samples);
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
    return;
  }
  void Controller::activate(VkCommandBuffer command) {
    auto &grid = m_job->grid;
    if (std::none_of(grid.probes.begin(), grid.probes.end(),
                     [](const auto &p) { return p.statistics[3] > .5f; }) &&
        m_job->surfaceReceived == 0)
      throw std::runtime_error("Bake contains no usable irradiance probes");
    auto volume = std::make_shared<Volume>();
    probes::dumpCaptures(m_hash, m_job->captures);
    volume->directDiffuse = m_job->hasDirectDiffuse;
    if (m_job->hasSun || std::any_of(grid.probes.begin(), grid.probes.end(),
                                    [](const auto &p) { return p.irradiance[7][3] > .5f; }))
      volume->sun = m_job->source->sun;
    volume->sunDepth = m_job->sunDepth;
    if (volume->sunDepth) volume->sunDepth->recordUpload(command);
    volume->hash = m_hash;
    volume->reflectionEvidence = m_job->reflectionEvidence;
    volume->localReflections = m_job->localReflections;
    if (!m_job->captures.empty()) {
      volume->content.captureInputs = m_job->captureInputHash;
      uint64_t captured = 1469598103934665603ull;
      for (const auto &capture : m_job->captures)
        for (const auto &color : capture.radiance)
          captured = scene::hashBytes(color.data(), 3 * sizeof(float), captured);
      volume->content.captureRadiance = captured | 1;
    }
    volume->sceneScale = m_source->sceneScale;
    volume->irradianceHeader = grid.header;
    volume->usableIrradianceProbes = uint32_t(std::count_if(
        grid.probes.begin(), grid.probes.end(), [](const Result &probe) { return probe.statistics[3] >= .5f; }));
    volume->mixedLights = std::make_shared<mixed::Bindings>(m_job->mixedLights->lights);
    mixed::dumpOwnership(*m_job->mixedLights, m_job->surfaceLayout);
    const size_t baseBytes = gridBytes(grid);
    const auto &publishedStyles = m_job->groupedStyles.styles.empty() ? m_job->styleWords : m_job->groupedStyles.words;
    const size_t styleBytes = publishedStyles.size() * sizeof(uint32_t);
    volume->styledLighting = styleBytes != 0;
    volume->groupedStyles = m_job->groupedStyles.styles;
    const size_t localOffset = (baseBytes + styleBytes + 15) & ~size_t(15);
    const auto localWords = m_job->localProbes ? m_job->localProbes->words() : std::vector<uint32_t>{};
    const size_t localBytes = localWords.size() * sizeof(uint32_t);
    const size_t bytes = localOffset + localBytes;
    const auto specular = probes::serialize(m_job->specular);
    probes::dumpPublishedData(".probes-published.bin", specular.data(), specular.size());
    probes::dumpPublishedData(".surfaces-published.bin", m_job->surfacePixels.data(), m_job->surfacePixels.size() * sizeof(uint16_t));
    probes::dumpPublishedData(".surface-charts.bin", m_job->surfaceLayout.charts.data(),
                              m_job->surfaceLayout.charts.size() * sizeof(lightmap::Allocation));
    const char *surfaceSamples = std::getenv("REMIX_DEBUG_BAKE_SURFACE_SAMPLES");
    if (surfaceSamples && std::strcmp(surfaceSamples, "1") == 0) {
      probes::dumpPublishedData(".surface-points.bin", m_job->surfaceLayout.points.data(),
                                m_job->surfaceLayout.points.size() * sizeof(Point));
      probes::dumpPublishedData(".surface-results.bin", m_job->surfaces.data(),
                                m_job->surfaces.size() * sizeof(Result));
      probes::dumpPublishedData(".surface-owners.bin", m_job->surfaceLayout.owners.data(),
                                m_job->surfaceLayout.owners.size() * sizeof(uint32_t));
    }
    const char *pageOverride = std::getenv("REMIX_DEBUG_MASK_PAGES");
    auto visibility = mixed::prepareVisibility(m_physical, m_device, *m_job->mixedLights,
        m_job->surfaceLayout, m_job->surfacePixels,
        !pageOverride || std::strcmp(pageOverride, "0") != 0,
        m_job->unifiedMasks ? &*m_job->unifiedMasks : nullptr,
        directionalFieldsRequested() ? std::span<const uint32_t>(m_job->directionalWords) : std::span<const uint32_t>{},
        directionalFieldsRequested() && m_job->completeDirectionals);
    auto &visibilityPages = visibility.pages;
    auto &occlusion = visibility.lookup;
    const auto compressedAllocation = visibility.compressedAllocation;
    volume->occlusion = std::move(visibility.lookupImage);
    volume->visibilityPages = std::move(visibility.pageImage);
    volume->surfaceDirectOffset = visibility.directionalOffset;
    const uint32_t surfaceLayers = lightmap::uploadedLayers(visibility.indexed, visibility.indexedDirect);
    const auto &surfaceLayout = m_job->surfaceLayout;
    const auto segments = lightmap::uploadSegments(surfaceLayout, visibility.indexed, visibility.indexedDirect);
    const size_t surfaceBytes = m_job->surfacePixels.empty() ? 0 : size_t(surfaceLayout.pageSize) *
        surfaceLayout.pageSize * surfaceLayout.pages * surfaceLayers * 4 * sizeof(uint16_t);
    std::fprintf(stderr, "Forward indexed visibility: %u, %llu -> %llu GPU bytes, %llu primary texels, max error %.9g\n",
        unsigned(visibility.indexed), static_cast<unsigned long long>(visibility.legacyAllocation),
        static_cast<unsigned long long>(visibility.totalAllocation),
        static_cast<unsigned long long>(visibility.convertedTexels), visibility.maximumError);
    std::fprintf(stderr, "Forward indexed directionality: %u, %zu bytes, table word %u, %u atlas layers, coverage=%s\n",
        unsigned(visibility.indexedDirect), visibility.indexedDirect ? m_job->directionalWords.size() * sizeof(uint32_t) : 0,
        visibility.directionalOffset, surfaceLayers,
        visibility.indexedDirect ? (m_job->completeDirectionals ? "complete" : "retained") : "legacy");
    const auto &maskCosts = m_job->mixedLights->masks.candidateCosts();
    if (maskCosts.candidates)
      probes::dumpPublishedData(".overflow-mask-costs.bin", &maskCosts, sizeof(maskCosts));
    probes::dumpPublishedData(".overflow-mask-entries.bin", m_job->mixedLights->masks.entries().data(),
                              m_job->mixedLights->masks.entries().size_bytes());
    probes::dumpPublishedData(".overflow-mask-bytes.bin", m_job->mixedLights->masks.bytes().data(),
                              m_job->mixedLights->masks.bytes().size_bytes());
    volume->occlusionParameters = {m_job->mixedLights->occlusion.wordsPerChart() |
                                       (visibility.indexed ? lightmap::indexedVisibilityFlag : 0u),
                                   m_job->mixedLights->occlusion.chartCount(), occlusion.width,
                                   visibility.hasMasks ? uint32_t(2 + m_job->mixedLights->occlusion.words().size()) : 0u};
    const size_t occlusionBytes = occlusion.words.size() * sizeof(uint32_t);
    volume->content.staticOcclusion = scene::fastHash(occlusion.words.data(), occlusionBytes) | 1;
    const size_t occlusionOffset = bytes + surfaceBytes;
    const size_t visibilityOffset = occlusionOffset + occlusionBytes;
    const size_t visibilityBytes = visibilityPages ? visibilityPages->texels.size() : 0;
    if (visibilityPages) {
      volume->content.staticOcclusion = scene::hashBytes(visibilityPages->texels.data(), visibilityBytes,
                                                        volume->content.staticOcclusion) | 1;
    }
    const uint64_t pageStatistics[] = {visibilityPages ? visibilityPages->size : 0,
        visibilityPages ? visibilityPages->layers : 0, visibilityBytes, occlusionBytes,
        compressedAllocation, volume->occlusion->bytes() +
            (volume->visibilityPages ? volume->visibilityPages->bytes() : 0)};
    probes::dumpPublishedData(".visibility-pages.bin", pageStatistics, sizeof(pageStatistics));
    std::vector<std::byte> data(bytes);
    auto gpuHeader = publicationHeader(grid.header, grid.lookup.size());
    gpuHeader.settings[3] = publicationSettings(0, false, !grid.depth.empty());
    std::memcpy(data.data(), &gpuHeader, sizeof(GridHeader));
    std::memcpy(data.data() + sizeof(GridHeader), grid.probes.data(), grid.probes.size() * sizeof(Result));
    if (!grid.lookup.empty())
      std::memcpy(data.data() + sizeof(GridHeader) + grid.probes.size() * sizeof(Result),
                  grid.lookup.data(), grid.lookup.size() * sizeof(uint32_t));
    if (!grid.depth.empty()) {
      grid.depth.validate(uint32_t(grid.probes.size()));
      const size_t offset = sizeof(GridHeader) + grid.probes.size() * sizeof(Result) + lookupBytes(grid);
      grid.depth.write(std::span(data).subspan(offset, grid.depth.bytes()));
    }
    // Hash once at publication, before releasing staging data. Diagnostics can
    // then distinguish input identity from changes in the actual baked result
    // without a GPU readback or a mutable worker-state dependency.
    probes::dumpPublishedData(".irradiance-published.bin", data.data(), baseBytes);
    volume->content.grid = scene::fastHash(data.data(), baseBytes) | 1;
    if (styleBytes) {
      volume->content.styledIndirect = scene::fastHash(publishedStyles.data(), styleBytes) | 1;
      std::memcpy(data.data() + baseBytes, publishedStyles.data(), styleBytes);
      auto header = gpuHeader;
      header.settings[3] = publicationSettings(0, true, !grid.depth.empty());
      std::memcpy(data.data(), &header, sizeof(header));
      probes::dumpPublishedData(".styled-irradiance.bin", publishedStyles.data(), styleBytes);
    }
    if (localBytes) {
      std::memcpy(data.data() + localOffset, localWords.data(), localBytes);
      auto header = gpuHeader;
      header.settings[3] = publicationSettings((localOffset - sizeof(GridHeader)) / 16, styleBytes != 0, !grid.depth.empty());
      std::memcpy(data.data(), &header, sizeof(header));
      volume->content.grid = scene::fastHash(localWords.data(), localBytes, volume->content.grid) | 1;
      probes::dumpPublishedData(".local-probe-visibility.bin", localWords.data(), localBytes);
    }
    volume->content.reflections = scene::fastHash(specular.data(), specular.size()) | 1;
    if (surfaceBytes) {
      if (!visibility.indexed) volume->content.surfaces = scene::fastHash(m_job->surfacePixels.data(), surfaceBytes) | 1;
      else {
        uint64_t hash = 0;
        for (const auto &segment : segments)
          hash = scene::fastHash(m_job->surfacePixels.data() + segment.source,
                                segment.values * sizeof(uint16_t), hash);
        volume->content.surfaces = hash | 1;
      }
    }
    if (surfaceBytes) {
      const auto &atlas = m_job->surfaceLayout;
      volume->surfaceAtlas = std::make_unique<Image>();
      volume->surfaceAtlas->create(
          m_physical, m_device, {atlas.pageSize, atlas.pageSize}, VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, atlas.pages * surfaceLayers);
      volume->surfaces = m_job->surfaceGeometry;
    }
    volume->staging = std::make_unique<Buffer>();
    volume->staging->create(m_physical, m_device, visibilityOffset + visibilityBytes,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    volume->staging->write(data.data(), data.size());
    if (surfaceBytes)
      for (const auto &segment : segments)
        volume->staging->write(m_job->surfacePixels.data() + segment.source,
            segment.values * sizeof(uint16_t), bytes + segment.destination * sizeof(uint16_t));
    volume->staging->write(occlusion.words.data(), occlusionBytes, occlusionOffset);
    if (visibilityBytes)
      volume->staging->write(visibilityPages->texels.data(), visibilityBytes, visibilityOffset);
    volume->data = std::make_unique<Buffer>();
    volume->data->create(m_physical, m_device, bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                             (ImageDump::enabled(m_physical) ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0u),
                         Buffer::Memory::Device);
    volume->specular = std::move(m_job->reflectionImage);
    if (!volume->specular) throw std::logic_error("Missing prepared reflection image");
    volume->reflectionHeader = volume->specular->header();
    const VkBufferCopy copy{0, 0, bytes};
    vkCmdCopyBuffer(command, volume->staging->handle, volume->data->handle, 1, &copy);
    volume->specular->recordUpload(command);
    if (volume->localReflections) volume->localReflections->image->recordUpload(command);
    VkImageMemoryBarrier surfaceReady{};
    volume->occlusion->upload(command, volume->staging->handle, occlusionOffset);
    if (volume->visibilityPages)
      volume->visibilityPages->upload(command, volume->staging->handle, visibilityOffset);
    if (volume->surfaceAtlas) {
      const auto &atlas = m_job->surfaceLayout;
      const VkImageMemoryBarrier transfer{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = volume->surfaceAtlas->handle,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, atlas.pages * surfaceLayers}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &transfer);
      const VkBufferImageCopy imageCopy{
          .bufferOffset = bytes,
          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, atlas.pages * surfaceLayers},
          .imageExtent = {atlas.pageSize, atlas.pageSize, 1}};
      vkCmdCopyBufferToImage(command, volume->staging->handle, volume->surfaceAtlas->handle,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopy);
      surfaceReady = transfer;
      surfaceReady.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      surfaceReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      surfaceReady.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      surfaceReady.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    const VkMemoryBarrier ready{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                         &ready, 0, nullptr, volume->surfaceAtlas ? 1u : 0u,
                         volume->surfaceAtlas ? &surfaceReady : nullptr);
    // The transfer and its visibility barrier precede this frame's surface
    // draws. Other frame slots keep their own previous volume alive.
    m_volume = std::move(volume);
    m_submissions[m_slot].upload = m_volume;
    char message[sizeof(m_statistics.status)];
    const char *label = m_job->cacheHit ? "Cached lighting active" : "Baked lighting active";
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
                                       "sites", "reflections", "geometry", "probe_depth"};
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
    if (!m_job->cacheHit && !m_job->cacheFile.empty()) {
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
        if (m_volume || m_statistics.state == REMIXAPI_BAKE_FAILED_EXT)
          return;
        if (!m_job) {
          if (!config.samples || config.samples > 8192 || !config.bounces || config.bounces > 8 ||
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
            if (job->cacheHit) {
              job->reflectionImage = std::make_unique<probes::ImageAsset>(
                  physical, device, job->specular, job->cancellation.get_token());
              job->restoreLocalReflections(physical, device, config);
              job->prepareLocalReflectionUpgrade(physical, device, config, queueFamily);
            }
          };
          job->ticket =
              m_pipelines->prepare("forward-bake-" + std::to_string(++generation), std::move(prepare));
          m_job = std::move(job);
          status(REMIXAPI_BAKE_PREPARING_EXT, "Preparing static bake resources");
        }
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
            const auto count = std::min(reference.hemisphere ? 256u : 1u, reference.tasks() - reference.next);
            const auto referencePoints = reference.batch(reference.next, count);
            const uint32_t firstSample = reference.sampleOffsets.empty() ? 0 : reference.sampleOffsets.at(reference.next);
            if (reference.hemisphere && reference.combinedLighting)
              job.transport->recordRadianceComponent(command, m_slot, referencePoints, Transport::combinedLighting,
                                                     config.bounces);
            else if (reference.hemisphere)
              job.transport->recordRadiance(command, m_slot, referencePoints, config.bounces);
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
          status(REMIXAPI_BAKE_TRACING_EXT, "Tracing offline irradiance reference");
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
          if (config.directionalProbeDepth && job.probeDepthReceived < job.grid.tasks.size())
            recordProbeDepth(command, config);
          else recordProbeSun(command, config);
          return;
        }
        auto &submission = m_submissions[m_slot];
        if (submission.job)
          throw std::logic_error("Bake slot reused before its fence");
        if (job.next < job.grid.tasks.size()) {
          const uint32_t count =
              job.count(Work::Irradiance, config, uint32_t(job.grid.tasks.size() - job.next), config.samples,
                        config.bounces);
          std::vector<Point> points;
          points.reserve(count);
          for (uint32_t i = 0; i < count; ++i)
            points.push_back(job.grid.points[job.grid.tasks[job.next + i]]);
          job.transport->recordBatch(command, m_slot, points, config.samples, config.bounces);
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
      status(REMIXAPI_BAKE_FAILED_EXT, error.what());
    }
    m_statistics.active = bool(m_volume);
    m_statistics.activeHash = m_volume ? m_volume->hash : 0;
    m_submissions[m_slot].sampled = m_volume;
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
    return (m_volume ? m_volume->specular : m_emptyReflections)->descriptors();
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
    auto addLocalImage = [&](const std::shared_ptr<const LocalReflections> &local) {
      if (local && local->image && localImages.insert(local->image.get()).second) {
        result.buffers += local->image->bufferBytes();
        result.images += local->image->imageBytes();
      }
    };
    std::unordered_set<const Job *> jobs;
    auto addJob = [&](const std::shared_ptr<Job> &job) {
      if (!job || !job->initialized || !jobs.insert(job.get()).second)
        return;
      addSurfaces(job->surfaceGeometry);
      addDepth(job->sunDepth);
      if (job->ticket->ready() && job->reflectionImage) {
        result.buffers += job->reflectionImage->bufferBytes();
        result.images += job->reflectionImage->imageBytes();
      }
      if (job->ticket->ready()) addLocalImage(job->localReflections);
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
      result.buffers += volume->specular->bufferBytes();
      result.images += volume->specular->imageBytes();
      if (volume->staging)
        result.buffers += volume->staging->allocationBytes;
    };
    addVolume(m_volume);
    for (const auto &submission : m_submissions) {
      addVolume(submission.upload);
      addVolume(submission.sampled);
    }
    return result;
  }
} // namespace remix::forward::bake
