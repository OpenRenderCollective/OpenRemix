#include "forward/probes/standalone_capture.h"
#include "forward/bake/transport_session.h"
#include "forward/bake/scene_request.h"
#include "forward/bake/transport_memory.h"
#include "forward/probes/automatic_capture.h"
#include "forward/probes/capture_cache.h"
#include "forward/probes/source2_reference.h"
#include "raster/renderer.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace remix::forward::probes {
namespace {
std::atomic_uint64_t generation = 0;
}
struct StandaloneCapture::Job : bake::TransportSession {
  pipeline::PreparationQueue::Ticket ticket;
  bool preparationReady() const { return (!ticket || ticket->ready()) && capture.preparationReady(); }
  void checkPreparation() const {
    if (ticket) ticket->check();
    capture.checkPreparation();
  }
  void cancelPreparation(pipeline::Service &service) {
    if (ticket) service.cancel(ticket);
    capture.cancelPreparation(service);
  }
  AutomaticCapture capture;
  std::vector<bake::Result> readback;
  std::filesystem::path cacheFile;
  uint64_t cacheKey = 0;
  std::unique_ptr<const capture_cache::Snapshot> persistence;
  bool cacheHit = false;
  bool prepared = false, filtering = false, failed = false;
};
StandaloneCapture::StandaloneCapture(VkPhysicalDevice physical, VkDevice device,
                                     pipeline::Service *pipelines,
                                     bool hardware, uint32_t queueFamily)
    : m_physical(physical), m_device(device), m_pipelines(pipelines),
      m_hardware(hardware), m_family(queueFamily) {}
StandaloneCapture::~StandaloneCapture() {
  if (m_job) {
    m_job->cancellation.request_stop();
    m_job->cancelPreparation(*m_pipelines);
  }
  for (const auto &job : m_retired) {
    job->cancellation.request_stop();
    job->cancelPreparation(*m_pipelines);
  }
}
void StandaloneCapture::retire() {
  if (!m_job)
    return;
  m_job->cancellation.request_stop();
  m_job->cancelPreparation(*m_pipelines);
  m_retired.push_back(std::move(m_job));
}
void StandaloneCapture::selectFrame(uint32_t slot) {
  m_slot = slot;
  auto &frame = m_frames.at(slot);
  if (frame.upload)
    frame.upload->releaseUpload();
  if (frame.job) {
    auto &job = *frame.job;
    if (frame.preparation)
      job.transport->releaseBuildInputs();
    if (frame.count && frame.job == m_job && !job.failed) {
      if (job.readback.size() < frame.count)
        job.readback.resize(frame.count);
      const auto results = std::span(job.readback).first(frame.count);
      job.transport->read(slot, results);
      if (!job.capture.observe({frame.step, frame.first, frame.count}, results))
        job.failed = true;
      if (const auto elapsed = job.transport->elapsed(slot)) {
        const auto work = AutomaticCapture::workType(frame.step);
        job.budget.observe(work, frame.count, *elapsed, m_config.gpuBudgetMs);
      }
    }
  }
  frame = {};
  std::erase_if(m_retired,
                [](const auto &job) { return job->preparationReady(); });
}
void StandaloneCapture::record(VkCommandBuffer command,
                               RasterRenderer &resources, CaptureConfig config,
                               uint32_t count, float detailTolerance, bake::SceneRequest *sceneRequest) {
  m_used.store(true, std::memory_order_relaxed);
  const auto source = resources.bakeScene();
  if (source != m_source || !config.sameLighting(m_config) ||
      count != m_count || detailTolerance != m_tolerance) {
    retire();
    m_active = {};
    m_cacheHit = false;
    m_status.clear();
    m_source = source;
    m_config = config;
    m_count = count;
    m_tolerance = detailTolerance;
  }
  m_config = config; // execution budget edits do not discard captured lighting
  auto &frame = m_frames[m_slot];
  frame.sampled = m_active.image;
  if (!config.enabled || !count) {
    retire();
    m_active = {};
    frame.sampled.reset();
    return;
  }
  if (!source || !source->ready || !source->contentHash || !m_hardware ||
      !m_pipelines)
    return;
  try {
    if (!m_job && !m_active.image && m_status.empty()) {
      if (count > maxProbes || config.reflectionResolution < 8 ||
          config.reflectionResolution > 512 ||
          !std::has_single_bit(config.reflectionResolution) ||
          !config.samples || config.samples > 8192 || !config.bounces ||
          config.bounces > 8 || !config.batch || config.batch > 1024 ||
          !std::isfinite(config.gpuBudgetMs) || config.gpuBudgetMs < 0 ||
          config.gpuBudgetMs > 16 || !std::isfinite(detailTolerance) ||
          detailTolerance < 0)
        throw std::invalid_argument(
            "Invalid standalone reflection configuration");
      auto job = std::make_shared<Job>();
      job->source = source;
      job->capture.reference = source2Reference(source->lightingHash(scene::BakeLightScope::Combined));
      const auto physical = m_physical;
      const auto device = m_device;
      const auto family = m_family;
      job->ticket = m_pipelines->prepare(
          "reflection-scene-" + std::to_string(++generation),
          [job, physical, device, family, config, count, detailTolerance] {
            VkPhysicalDeviceProperties limits;
            vkGetPhysicalDeviceProperties(physical, &limits);
            const auto capacity = imageCapacity(
                config.reflectionResolution, limits.limits.maxImageDimension2D,
                limits.limits.maxImageArrayLayers,
                limits.limits.maxStorageBufferRange);
            if (count > capacity)
              throw std::length_error(
                  "Standalone reflection set exceeds image capacity");
            if (config.diskCache) {
              capture_cache::Device deviceIdentity{
                  limits.vendorID,
                  limits.deviceID,
                  limits.driverVersion,
                  {count, config.reflectionResolution,
                   limits.limits.maxImageDimension2D,
                   limits.limits.maxImageArrayLayers,
                   limits.limits.maxStorageBufferRange}};
              std::copy_n(limits.pipelineCacheUUID, deviceIdentity.uuid.size(),
                          deviceIdentity.uuid.begin());
              job->cacheKey = capture_cache::standaloneKey(
                  job->source->contentHash,
                  bake::Transport::shaderFingerprint(), config, count,
                  detailTolerance, deviceIdentity);
              try {
                job->cacheFile = capture_cache::standalonePath(job->cacheKey);
              } catch (const std::exception &) {
              }
              if (!job->cacheFile.empty()) {
                auto cached = capture_cache::read(
                    job->cacheFile, job->cacheKey, deviceIdentity.limits,
                    job->cancellation.get_token());
                if (cached) {
                  job->capture.captures = std::move(cached->captures);
                  job->capture.filtered = std::move(cached->data);
                  job->capture.cachedRadiance = true;
                  job->capture.finishImageOnlyPublication(physical, device, job->cancellation.get_token());
                  job->cacheHit = job->filtering = true;
                  return;
                }
              }
            }
            job->capture.prepareNext(*job->source, {.requested = count},
                                      job->cancellation.get_token());
            bake::Options options;
            options.queueFamily = family;
            options.textureResolution = config.textureResolution;
            options.batchPoints = bake::Budget::maximumPoints;
            options.rayBudget = bake::Budget::maximumWork;
            options.initialBatchPoints = 1024;
            auto transport = std::make_unique<bake::Transport>();
            transport->initialize(physical, device, job->source, options);
            job->transport = std::move(transport);
          });
      m_job = std::move(job);
    }
    if (!m_job || !m_job->preparationReady())
      return;
    auto &job = *m_job;
    job.checkPreparation();
    if (job.failed)
      throw std::runtime_error("Invalid standalone reflection readback");
    if (sceneRequest && job.transport) sceneRequest->offer(*job.transport);
    if (job.filtering) {
      frame.upload = job.capture.recordImageUpload(command);
      if (!frame.upload) throw std::logic_error("Standalone reflection image was not prepared");
      m_active = {job.source->contentHash, job.capture.image};
      m_cacheHit = job.cacheHit;
      frame.sampled = m_active.image;
      if (config.diskCache && job.persistence && !job.cacheFile.empty()) {
        try {
          (void)capture_cache::persistence().submit(
              job.cacheFile, job.cacheKey, std::move(job.persistence));
        } catch (const std::exception &) {
        } // optional disk writes never revoke publication
      }
      m_job.reset();
      return;
    }
    if (!job.prepared) {
      job.transport->recordPrepare(command, resources);
      job.prepared = true;
      frame.job = m_job;
      frame.preparation = true;
      return;
    }
    const auto step = job.capture.nextStep();
    const auto owner = m_job;
    if (step == AutomaticCapture::Step::Fit) {
      job.capture.prepare(
          *m_pipelines, "reflection-fit-" + std::to_string(++generation),
          [owner, count, detailTolerance] {
            owner->capture.prepareNext(*owner->source,
                {.requested = count, .detailTolerance = detailTolerance}, owner->cancellation.get_token());
          });
    } else if (step == AutomaticCapture::Step::Select) {
      job.capture.prepare(
          *m_pipelines, "reflection-select-" + std::to_string(++generation),
          [owner, count, config, detailTolerance] {
            owner->capture.prepareNext(*owner->source,
                {count, count, config.reflectionResolution, detailTolerance}, owner->cancellation.get_token());
            owner->capture.prepareRadiance(config, {}, owner->cancellation.get_token());
          });
    } else if (step == AutomaticCapture::Step::Complete) {
      if (job.capture.captures.empty()) {
        m_status = "No admitted reflection capture sites";
        retire();
        return;
      }
      job.filtering = true;
      const auto physical = m_physical;
      const auto device = m_device;
      job.capture.prepare(
          *m_pipelines, "reflection-filter-" + std::to_string(++generation),
          [owner, physical, device] {
            owner->capture.complete(owner->cancellation.get_token());
            if (owner->cacheKey) {
              auto snapshot = owner->capture.finishTransferredPublication(physical, device,
                                                                            owner->cancellation.get_token());
              if (capture_cache::retainedCharge(*snapshot) <= capture_cache::maximumBytes)
                owner->persistence = std::move(snapshot);
              // Declined payloads release CPU storage on this worker.
            } else {
              owner->capture.finishImageOnlyPublication(physical, device, owner->cancellation.get_token());
            }
          });
    } else {
      if (frame.job)
        throw std::logic_error(
            "Standalone reflection slot reused before fence");
      const auto dispatch = job.capture.record(command, m_slot, *job.transport,
                                               job.budget, config);
      if (dispatch.count) {
        frame.job = m_job;
        frame.first = dispatch.first;
        frame.count = dispatch.count;
        frame.step = dispatch.step;
      }
    }
  } catch (VkResult error) {
    m_status =
        "Standalone reflection Vulkan failure " + std::to_string(int(error));
    retire();
  } catch (const std::exception &error) {
    m_status = error.what();
    retire();
  }
}
float StandaloneCapture::sceneScale() const {
  return m_source ? m_source->sceneScale : 1;
}
StandaloneCapture::Memory StandaloneCapture::memory(bake::TransportMemory *shared) const {
  bake::TransportMemory local;
  auto &transportMemory = shared ? *shared : local;
  Memory result;
  std::unordered_set<const Job *> jobs;
  std::unordered_set<const ImageAsset *> images;
  auto addImage = [&](const auto &image) {
    if (image && images.insert(image.get()).second) {
      result.buffers += image->bufferBytes();
      result.images += image->imageBytes();
    }
  };
  auto addJob = [&](const auto &job) {
    if (!job || !jobs.insert(job.get()).second || !job->preparationReady())
      return;
    if (job->transport) {
      const auto bytes = transportMemory.add(*job->transport);
      result.images += bytes.images;
      result.acceleration += bytes.acceleration;
      result.buffers += bytes.buffers;
    }
    addImage(job->capture.image);
  };
  addJob(m_job);
  for (const auto &job : m_retired)
    addJob(job);
  addImage(m_active.image);
  for (const auto &frame : m_frames) {
    addJob(frame.job);
    addImage(frame.sampled);
    addImage(frame.upload);
  }
  return result;
}
} // namespace remix::forward::probes
