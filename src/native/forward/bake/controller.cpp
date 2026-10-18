#include "forward/bake/job.h"
#include "forward/bake/budget.h"
#include "forward/bake/cache.h"
#include "forward/bake/receiver_report.h"
#include "forward/bake/transport.h"
#include "forward/image_dump.h"
#include "forward/lightmap/denoise.h"
#include "forward/mixed/overflow.h"
#include "forward/mixed/mask_pages.h"
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
  bool Controller::Job::consumeVisibility(uint32_t slot, uint32_t first, uint32_t count, bool local,
                                         bool overflow) {
    if (overflow) {
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
    for (uint32_t i = 0; i < count; ++i) {
      const auto &value = results[i];
      if (!std::isfinite(value.visibility) ||
          !std::all_of(value.irradiance.begin(), value.irradiance.end(),
                       [](float coefficient) { return std::isfinite(coefficient); }))
        return false;
      if (local) {
        const auto &task = mixedLights->tasks[first + i];
        visibility[task.point][task.channel] = std::clamp(value.visibility, 0.f, 1.f);
        if (!direct.empty())
          for (uint32_t coefficient = 0; coefficient < lightmap::irradianceLayers; ++coefficient)
            directScratch[task.point][coefficient][task.channel] = value.irradiance[coefficient];
      } else {
        auto &output = sun[first + i];
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
    const char *samples = std::getenv("REMIX_DEBUG_BAKE_SURFACE_SAMPLES");
    if (config.diskCache && (!samples || std::strcmp(samples, "1") != 0)) {
      cache::Device deviceIdentity{properties.vendorID, properties.deviceID, properties.driverVersion,
                                     properties.limits.maxImageDimension2D,
                                     properties.limits.maxImageArrayLayers,
                                     properties.limits.maxStorageBufferRange,
                                     {}};
      std::copy_n(properties.pipelineCacheUUID, deviceIdentity.uuid.size(), deviceIdentity.uuid.begin());
      cacheKey = cache::key(source->contentHash, config, lightCutoff, deviceIdentity);
      try {
        cacheFile = cache::path(cacheKey);
      } catch (const std::exception &) {
        cacheFile.clear();
      }
      if (!cacheFile.empty()) {
        auto cached = cache::read(cacheFile, cacheKey, source, config, deviceIdentity, cancellation.get_token());
        if (!cancellation.stop_requested())
          cache::recordLookup(cacheFile, cacheKey, source->contentHash,
                               cache::key(0, config, lightCutoff, deviceIdentity), bool(cached));
        if (cached) {
          grid = std::move(cached->grid);
          surfaceLayout = std::move(cached->layout);
          mixedLights = std::make_shared<mixed::Plan>(std::move(cached->mixed));
          captures = std::move(cached->captures);
          specular = std::move(cached->specular);
          surfacePixels = std::move(cached->pixels);
          styleWords = std::move(cached->styles);
          surfaceReceived = cached->surfaceSamples;
          hasDirectDiffuse = cached->directDiffuse;
          hasSun = cached->sun;
          received = uint32_t(grid.tasks.size());
          for (const auto &capture : captures) {
            const auto rays = probes::rays(capture);
            captureRays.insert(captureRays.end(), rays.begin(), rays.end());
          }
          if (!source->charts.empty())
            surfaceGeometry = std::make_shared<lightmap::Geometry>(
                physical, device, source, surfaceLayout, cancellation.get_token(),
                mixedLights->charts, mixedLights->directCharts);
          dumpReceiverReport(*source, surfaceLayout, surfacePixels, grid, cancellation.get_token());
          cacheHit = filtering = true;
          return;
        }
      }
    }
    if (cancellation.stop_requested())
      throw std::runtime_error("Bake preparation superseded");
    auto placement = config;
    placement.maxProbes =
        std::min(placement.maxProbes,
                 uint32_t((properties.limits.maxStorageBufferRange - sizeof(GridHeader)) / sizeof(Result)));
    grid = place(*source, placement);
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
    if (config.relocateProbes) {
      const auto relocated = relocate(grid, *source, surfaceLayout, cancellation.get_token());
      if (std::getenv("REMIX_DEBUG_BAKE_RECEIVERS"))
        std::fprintf(stderr, "Forward probe relocation: %u slots, %zu/%zu trace origins\n",
                     relocated, grid.tasks.size(), grid.points.size());
    }
    surfaces.resize(surfaceLayout.points.size());
    Options options;
    mixedLights = std::make_shared<mixed::Plan>(
        mixed::plan(source, surfaceLayout, lightCutoff, cancellation.get_token(),
                    options.bias / source->sceneScale));
    if (std::getenv("REMIX_DEBUG_BAKE_CAPTURE_PREFIX")) {
      uint64_t candidates = mixedLights->overflow;
      for (const auto &chart : mixedLights->charts)
        for (auto light : chart)
          candidates += light != mixed::none;
      std::fprintf(stderr, "OpenRemix mixed plan: candidates=%llu hemisphere_rejected=%llu initial_tasks=%zu\n",
                   static_cast<unsigned long long>(candidates),
                   static_cast<unsigned long long>(mixedLights->hemisphereRejectedPairs), mixedLights->tasks.size());
    }
    visibility.resize(surfaceLayout.points.size(), {1, 1, 1, 1});
    if (source->sun.isDistant)
      sun.resize(surfaceLayout.points.size());
    if (config.directDiffuse && !mixedLights->tasks.empty()) {
      direct.resize(surfaceLayout.points.size());
      directScratch.resize(surfaceLayout.points.size());
    }
    hasSun = !sun.empty();
    hasDirectDiffuse = config.directDiffuse && (!direct.empty() || hasSun);
    options.batchPoints = Budget::maximumPoints;
    options.rayBudget = Budget::maximumWork;
    options.initialBatchPoints = 1024;
    options.queueFamily = queueFamily;
    options.textureResolution = config.textureResolution;
    options.lightingComponents = config.styledIndirect && !styleIds(*source).empty();
    transport = std::make_unique<Transport>();
    transport->initialize(physical, device, source, options);
  }
  Controller::Controller(VkPhysicalDevice physical, VkDevice device, pipeline::Service *pipelines,
                         bool hardware, uint32_t queueFamily)
      : m_physical(physical), m_device(device), m_pipelines(pipelines), m_queueFamily(queueFamily) {
    m_empty = std::make_unique<Buffer>();
    m_empty->create(physical, device, sizeof(probes::Header), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const probes::Header empty{};
    m_empty->write(&empty, sizeof(empty));
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
    if (submission.upload)
      submission.upload->staging.reset();
    if (submission.job) {
      auto &job = *submission.job;
      if (submission.preparation)
        job.transport->releaseBuildInputs();
      if (submission.count && submission.job == m_job && !job.failed) {
        const auto readBegin = std::chrono::steady_clock::now();
        if (submission.visibility || submission.sun || submission.overflowVisibility) {
          if (!job.consumeVisibility(slot, submission.first, submission.count, submission.visibility,
                                     submission.overflowVisibility)) {
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
                const uint32_t count = uint32_t(job.captures.front().radiance.size());
                const uint32_t ray = submission.first + i;
                auto color = value.irradiance[0];
                for (uint32_t c = 0; c < 3; ++c)
                  color[c] = std::clamp(color[c], 0.f, 65000.f);
                job.captures[ray / count].radiance[ray % count] = color;
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
        job.readbackMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readBegin).count();
        job.readbackBytes += uint64_t(submission.count) *
                             (submission.overflowVisibility ? sizeof(float) :
                              submission.visibility || submission.sun
                                  ? sizeof(VisibilityResult) : sizeof(Result));
        if (!submission.styled) (submission.radiance             ? job.captureReceived
         : submission.reflectionGeometry ? job.geometryReceived
         : submission.reflectionSite     ? job.siteReceived
         : submission.visibility         ? job.visibilityReceived
         : submission.overflowVisibility ? job.overflowReceived
         : submission.sun                ? job.sunReceived
         : submission.surface            ? job.surfaceReceived
                                         : job.received) += submission.count;
        if (const auto elapsed = job.transport->elapsed(slot)) {
          const auto work = submission.reflectionGeometry                            ? Work::Geometry
                            : submission.visibility || submission.overflowVisibility ? Work::Visibility
                            : submission.sun                                         ? Work::Sun
                            : submission.reflectionSite                              ? Work::Site
                            : submission.radiance                                    ? Work::Reflection
                            : submission.surface                                     ? Work::Surface
                                                                                     : Work::Irradiance;
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
  void Controller::recordSun(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (job.sunReceived == job.sun.size()) {
      recordReflections(command, config);
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
  void Controller::recordVisibility(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (!job.visibilityFinished && job.visibilityReceived == job.mixedLights->tasks.size()) {
      job.visibilityFinished = !mixed::refine(*job.mixedLights, job.surfaceLayout, job.visibility,
                                              job.cancellation.get_token(), job.directScratch, job.direct);
      if (job.visibilityFinished)
        releaseStorage(job.directScratch);
      job.visibilityNext = job.visibilityReceived = 0;
    }
    if (job.visibilityFinished) {
      recordOverflowVisibility(command, config);
      return;
    }
    if (job.visibilityNext < job.mixedLights->tasks.size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Mixed visibility slot reused before its fence");
      const uint32_t count =
          job.count(Work::Visibility, config, uint32_t(job.mixedLights->tasks.size() - job.visibilityNext),
                    config.samples);
      std::vector<VisibilityPoint> points;
      for (uint32_t i = 0; i < count; ++i) {
        const auto &task = job.mixedLights->tasks[job.visibilityNext + i];
        points.push_back({job.surfaceLayout.points[task.point], task.light});
      }
      job.transport->recordVisibility(command, m_slot, points, config.samples, config.directDiffuse);
      submission.job = m_job;
      submission.visibility = true;
      submission.first = job.visibilityNext;
      submission.count = count;
      job.visibilityNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Baking per-light static visibility (%u/%zu samples)",
                  job.visibilityReceived, job.mixedLights->tasks.size());
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
      recordSun(command, config);
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
        owner->overflowFinished = !owner->overflowAudit->nextRound(
            *owner->mixedLights, owner->surfaceLayout, owner->cancellation.get_token());
        owner->overflowNext = owner->overflowReceived = 0;
      });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Preparing overflow visibility masks");
      return;
    }
    const auto tasks = job.overflowAudit->tasks();
    if (job.overflowNext < tasks.size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Overflow visibility slot reused before its fence");
      const uint32_t count = job.count(Work::Visibility, config,
                                       uint32_t(tasks.size() - job.overflowNext), config.samples);
      std::vector<VisibilityPoint> points;
      points.reserve(count);
      for (const auto &task : tasks.subspan(job.overflowNext, count))
        points.push_back({job.surfaceLayout.points[task.point], task.light});
      // Classification needs visibility only, not another set of directional SH.
      job.transport->recordVisibility(command, m_slot, points, config.samples, false);
      submission.job = m_job;
      submission.overflowVisibility = true;
      submission.first = job.overflowNext;
      submission.count = count;
      job.overflowNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Checking overflow light visibility (%llu/%llu surface/light pairs)",
                  static_cast<unsigned long long>(job.overflowAudit->completedWork()),
                  static_cast<unsigned long long>(job.overflowAudit->totalWork()));
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
  void Controller::recordReflections(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
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
      job.ticket =
          m_pipelines->prepare("forward-probe-selection-" + std::to_string(++generation), [owner, config] {
            if (owner->projectionAudit) {
              owner->candidateRegions = owner->projectionAudit->finish();
              owner->projectionAudit.reset();
            }
            owner->captures = probes::captureCandidates(
                *owner->source, owner->candidateRegions, config.reflectionProbes, config.reflectionResolution,
                owner->cancellation.get_token(), config.reflectionDetailTolerance);
            probes::dumpCandidates(owner->source->contentHash, owner->candidateRegions, owner->captures);
            releaseStorage(owner->candidateRegions);
            for (const auto &capture : owner->captures) {
              auto rays = probes::rays(capture);
              owner->captureRays.insert(owner->captureRays.end(), rays.begin(), rays.end());
            }
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Selecting reflection captures");
      return;
    }
    if (job.captureReceived == job.captureRays.size()) {
      job.filtering = true;
      const auto owner = m_job;
      const auto physical = m_physical;
      const auto device = m_device;
      job.ticket = m_pipelines->prepare(
          "forward-probe-filter-" + std::to_string(++generation), [owner, physical, device, config] {
            owner->finishStage.store(Job::FinishStage::Reflections, std::memory_order_relaxed);
            owner->specular = probes::filter(owner->captures, 128, owner->cancellation.get_token());
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
              std::fprintf(stderr, "Forward lightmap OIDN host time: initialize %.3f s, prepare %.3f s, "
                                   "upload %.3f s, execute %.3f s, readback %.3f s, wait %.3f s, chart %.3f s\n",
                           filtered.initializationSeconds, filtered.preparationSeconds,
                           filtered.uploadSeconds, filtered.executionSeconds, filtered.readbackSeconds,
                           filtered.waitSeconds, filtered.chartSeconds);
              std::fprintf(stderr, "Forward lightmap OIDN staging: %llu pinned bytes, %.3f s host setup/copies\n",
                           static_cast<unsigned long long>(filtered.stagingBufferBytes), filtered.stagingSeconds);
            }
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
              const uint64_t baseBytes = sizeof(GridHeader) + owner->grid.probes.size() * sizeof(Result);
              if (baseBytes + 16 >= limits.limits.maxStorageBufferRange)
                throw std::length_error("No device storage range for attributed lighting");
              const uint64_t maximum = std::min(uint64_t(config.styleStorageMiB) << 20,
                  uint64_t(limits.limits.maxStorageBufferRange) - baseBytes - 15);
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
            owner->finishStage.store(Job::FinishStage::Cache, std::memory_order_relaxed);
            owner->baseFinished = true;
            owner->filtering = !owner->styles;
            if (owner->filtering) owner->saveCache();
          });
      status(REMIXAPI_BAKE_PREPARING_EXT, "Finishing baked lighting assets");
      return;
    }
    if (job.captureNext < job.captureRays.size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job)
        throw std::logic_error("Reflection slot reused before its fence");
      const uint32_t count = job.count(Work::Reflection, config,
                                       uint32_t(job.captureRays.size() - job.captureNext),
                                       config.samples, config.bounces);
      job.transport->recordRadiance(
          command, m_slot, std::span(job.captureRays).subspan(job.captureNext, count), config.bounces, config.samples);
      submission.job = m_job;
      submission.radiance = true;
      submission.first = job.captureNext;
      submission.count = count;
      job.captureNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Capturing reflection cubemaps (%u/%zu texels, %u samples)",
                  job.captureReceived, job.captureRays.size(), config.samples);
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
    if (m_job->hasSun)
      volume->sun = m_job->source->sun;
    volume->hash = m_hash;
    volume->reflectionHeader = m_job->specular.header;
    if (!m_job->captures.empty()) {
      volume->content.captureInputs =
          scene::fastHash(m_job->captureRays.data(), m_job->captureRays.size() * sizeof(Point)) | 1;
      uint64_t captured = 1469598103934665603ull;
      for (const auto &capture : m_job->captures)
        for (const auto &color : capture.radiance)
          captured = scene::hashBytes(color.data(), 3 * sizeof(float), captured);
      volume->content.captureRadiance = captured | 1;
    }
    volume->sceneScale = m_source->sceneScale;
    volume->mixedLights = std::make_shared<mixed::Bindings>(m_job->mixedLights->lights);
    const size_t baseBytes = sizeof(GridHeader) + grid.probes.size() * sizeof(Result);
    const size_t styleBytes = m_job->styleWords.size() * sizeof(uint32_t);
    volume->styledIndirect = styleBytes != 0;
    const size_t bytes = (baseBytes + styleBytes + 15) & ~size_t(15);
    const auto specular = probes::serialize(m_job->specular);
    probes::dumpPublishedData(".probes-published.bin", specular.data(), specular.size());
    const size_t surfaceBytes = m_job->surfacePixels.size() * sizeof(uint16_t);
    probes::dumpPublishedData(".surfaces-published.bin", m_job->surfacePixels.data(), surfaceBytes);
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
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(m_physical, &properties);
    const auto chartCount = uint32_t(m_job->surfaceLayout.charts.size());
    VkFormatProperties maskFormat;
    vkGetPhysicalDeviceFormatProperties(m_physical, VK_FORMAT_R8_UNORM, &maskFormat);
    std::optional<mixed::MaskPages> visibilityPages;
    const char *pageOverride = std::getenv("REMIX_DEBUG_MASK_PAGES");
    if ((!pageOverride || std::strcmp(pageOverride, "0") != 0) &&
        (maskFormat.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
      visibilityPages = mixed::selectMaskPages(m_job->mixedLights->masks, chartCount,
          properties.limits.maxImageDimension2D, properties.limits.maxImageArrayLayers);
    auto masks = m_job->mixedLights->masks.words(chartCount);
    const auto &maskCosts = m_job->mixedLights->masks.candidateCosts();
    if (maskCosts.candidates)
      probes::dumpPublishedData(".overflow-mask-costs.bin", &maskCosts, sizeof(maskCosts));
    probes::dumpPublishedData(".overflow-mask-entries.bin", m_job->mixedLights->masks.entries().data(),
                              m_job->mixedLights->masks.entries().size_bytes());
    probes::dumpPublishedData(".overflow-mask-bytes.bin", m_job->mixedLights->masks.bytes().data(),
                              m_job->mixedLights->masks.bytes().size_bytes());
    auto occlusion = m_job->mixedLights->occlusion.pixels(properties.limits.maxImageDimension2D, masks);
    volume->occlusion = std::make_unique<mixed::OcclusionMap>(m_physical, m_device,
        VkExtent2D{occlusion.width, occlusion.height});
    const uint64_t compressedAllocation = volume->occlusion->bytes();
    if (visibilityPages) {
      auto pageWords = visibilityPages->words(chartCount);
      auto pageLookup = m_job->mixedLights->occlusion.pixels(properties.limits.maxImageDimension2D, pageWords);
      auto lookupImage = std::make_unique<mixed::OcclusionMap>(m_physical, m_device,
          VkExtent2D{pageLookup.width, pageLookup.height});
      auto pageImage = std::make_unique<mixed::OcclusionMap>(m_physical, m_device,
          VkExtent2D{visibilityPages->size, visibilityPages->size}, VK_FORMAT_R8_UNORM, visibilityPages->layers);
      // Allocation alignment can outweigh logical savings on small maps.
      // Neither candidate has been submitted; discard it if actual device
      // allocations do not improve on the compressed image.
      if (lookupImage->bytes() + pageImage->bytes() < compressedAllocation) {
        masks = std::move(pageWords);
        occlusion = std::move(pageLookup);
        volume->occlusion = std::move(lookupImage);
        volume->visibilityPages = std::move(pageImage);
      } else {
        visibilityPages.reset();
      }
    }
    volume->occlusionParameters = {m_job->mixedLights->occlusion.wordsPerChart(),
                                   m_job->mixedLights->occlusion.chartCount(), occlusion.width,
                                   masks.empty() ? 0u : uint32_t(2 + m_job->mixedLights->occlusion.words().size())};
    const size_t occlusionBytes = occlusion.rgba.size() * sizeof(uint16_t);
    volume->content.staticOcclusion = scene::fastHash(occlusion.rgba.data(), occlusionBytes) | 1;
    const size_t occlusionOffset = bytes + specular.size() + surfaceBytes;
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
    std::memcpy(data.data(), &grid.header, sizeof(GridHeader));
    std::memcpy(data.data() + sizeof(GridHeader), grid.probes.data(), grid.probes.size() * sizeof(Result));
    // Hash once at publication, before releasing staging data. Diagnostics can
    // then distinguish input identity from changes in the actual baked result
    // without a GPU readback or a mutable worker-state dependency.
    probes::dumpPublishedData(".irradiance-published.bin", data.data(), baseBytes);
    volume->content.grid = scene::fastHash(data.data(), baseBytes) | 1;
    if (styleBytes) {
      volume->content.styledIndirect = scene::fastHash(m_job->styleWords.data(), styleBytes) | 1;
      std::memcpy(data.data() + baseBytes, m_job->styleWords.data(), styleBytes);
      auto header = grid.header;
      header.settings[3] = 1;
      std::memcpy(data.data(), &header, sizeof(header));
      probes::dumpPublishedData(".styled-irradiance.bin", m_job->styleWords.data(), styleBytes);
    }
    volume->content.reflections = scene::fastHash(specular.data(), specular.size()) | 1;
    if (surfaceBytes)
      volume->content.surfaces = scene::fastHash(m_job->surfacePixels.data(), surfaceBytes) | 1;
    if (surfaceBytes) {
      const auto &atlas = m_job->surfaceLayout;
      volume->surfaceAtlas = std::make_unique<Image>();
      volume->surfaceAtlas->create(
          m_physical, m_device, {atlas.pageSize, atlas.pageSize}, VK_FORMAT_R16G16B16A16_SFLOAT,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT, atlas.pages * lightmap::atlasLayers);
      volume->surfaces = m_job->surfaceGeometry;
    }
    volume->staging = std::make_unique<Buffer>();
    volume->staging->create(m_physical, m_device, visibilityOffset + visibilityBytes,
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    volume->staging->write(data.data(), data.size());
    volume->staging->write(specular.data(), specular.size(), bytes);
    volume->staging->write(m_job->surfacePixels.data(), surfaceBytes, bytes + specular.size());
    volume->staging->write(occlusion.rgba.data(), occlusionBytes, occlusionOffset);
    if (visibilityBytes)
      volume->staging->write(visibilityPages->texels.data(), visibilityBytes, visibilityOffset);
    volume->data = std::make_unique<Buffer>();
    volume->data->create(m_physical, m_device, bytes,
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                             (ImageDump::enabled(m_physical) ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0u),
                         Buffer::Memory::Device);
    volume->specular = std::make_unique<Buffer>();
    volume->specular->create(m_physical, m_device, specular.size(),
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                 (ImageDump::enabled(m_physical) ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : 0u),
                             Buffer::Memory::Device);
    const VkBufferCopy copy{0, 0, bytes};
    vkCmdCopyBuffer(command, volume->staging->handle, volume->data->handle, 1, &copy);
    const VkBufferCopy reflectionCopy{bytes, 0, specular.size()};
    vkCmdCopyBuffer(command, volume->staging->handle, volume->specular->handle, 1, &reflectionCopy);
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
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, atlas.pages * lightmap::atlasLayers}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &transfer);
      const VkBufferImageCopy imageCopy{
          .bufferOffset = bytes + specular.size(),
          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, atlas.pages * lightmap::atlasLayers},
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
      constexpr const char *names[] = {"grid",  "surface",     "visibility", "sun",
                                       "sites", "reflections", "geometry"};
      for (uint32_t i = 0; i < uint32_t(Work::Count); ++i) {
        const auto &timing = m_job->budget.statistics(Work(i));
        std::fprintf(stderr, "Bake GPU timing: %s points=%llu batches=%llu total_ms=%.3f "
                             "longest_ms=%.3f largest_batch=%u\n",
                     names[i], static_cast<unsigned long long>(timing.points),
                     static_cast<unsigned long long>(timing.batches), timing.milliseconds,
                     timing.longest, timing.largest);
      }
    }
    m_job.reset();
    status(REMIXAPI_BAKE_READY_EXT, message);
  }
  void Controller::record(VkCommandBuffer command, RasterRenderer &resources, const Config &config,
                          float lightCutoff) {
    std::erase_if(m_obsolete, [](const auto &job) { return job->ticket->ready(); });
    if (!m_emptySurfaceReady) {
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
        if (m_volume || m_statistics.state == REMIXAPI_BAKE_FAILED_EXT)
          return;
        if (!m_job) {
          if (!config.samples || config.samples > 8192 || !config.bounces || config.bounces > 8 ||
              !config.batch || config.batch > 1024 ||
              (config.styledIndirect && (!config.styleStorageMiB || config.styleStorageMiB > 512)))
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
            case Job::FinishStage::Reflections: message = "Filtering reflection cubemaps"; break;
            case Job::FinishStage::Denoising: message = "Denoising surface lightmaps"; break;
            case Job::FinishStage::Atlas: message = "Encoding surface lightmaps"; break;
            case Job::FinishStage::Geometry: message = "Preparing lightmapped geometry"; break;
            case Job::FinishStage::Cache: message = "Saving baked lighting cache"; break;
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
          job.prepared = true;
          submission.job = m_job;
          submission.preparation = true;
          status(REMIXAPI_BAKE_TRACING_EXT, "Preparing static transport on GPU");
          return;
        }
        if (job.received == job.grid.tasks.size()) {
          recordSurfaces(command, config);
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
  VkDescriptorBufferInfo Controller::reflectionDescriptor() const {
    return m_volume ? m_volume->specular->descriptor() : m_empty->descriptor();
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
    result.buffers = m_empty->allocationBytes;
    result.images = m_emptySurface->bytes + m_emptyOcclusion->bytes();
    std::unordered_set<const lightmap::Geometry *> surfaces;
    auto addSurfaces = [&](const std::shared_ptr<lightmap::Geometry> &geometry) {
      if (geometry && surfaces.insert(geometry.get()).second)
        result.buffers += geometry->memoryBytes();
    };
    std::unordered_set<const Job *> jobs;
    auto addJob = [&](const std::shared_ptr<Job> &job) {
      if (!job || !job->initialized || !jobs.insert(job.get()).second)
        return;
      addSurfaces(job->surfaceGeometry);
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
      if (volume->surfaceAtlas)
        result.images += volume->surfaceAtlas->bytes;
      if (volume->occlusion)
        result.images += volume->occlusion->bytes();
      if (volume->visibilityPages)
        result.images += volume->visibilityPages->bytes();
      result.buffers += volume->data->allocationBytes;
      result.buffers += volume->specular->allocationBytes;
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
