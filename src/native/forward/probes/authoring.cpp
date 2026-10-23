#include "forward/probes/authoring.h"
#include "forward/probes/identities.h"
#include "platform/workers.h"
#include "forward/bake/transport.h"
#include "forward/bake/budget.h"
#include "forward/bake/controller.h"
#include "raster/renderer.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <stop_token>
#include <stdexcept>
#include <unordered_set>

namespace remix::forward::probes {
  namespace { std::atomic_uint64_t generation = 0; }
  struct Authoring::Job {
    pipeline::PreparationQueue::Ticket ticket;
    std::stop_source cancellation;
    std::shared_ptr<const scene::ReflectionVolumes> request;
    std::vector<Capture> captures;
    std::vector<bake::Point> rays;
    std::vector<uint32_t> rayOwners; // one owner per complete six-face capture
    std::shared_ptr<bake::Transport> transport;
    std::unique_ptr<ImageAsset> image;
    std::unique_ptr<ImageAsset> authoredImage;
    std::shared_ptr<const bake::Volume> automatic;
    uint64_t revision = 0;
    uint32_t automaticCount = 0, covered = 0, omitted = 0;
    Data filtered;
    LayerIdentities identities{};
    bake::Budget budget;
    std::vector<int32_t> reused, aliases;
    std::shared_ptr<Job> previous;
    uint32_t next = 0, received = 0, rayCount = 0;
    bool prepared = false, filtering = false, sceneReused = false;
    void prepare(const std::shared_ptr<Job> &prior, const std::shared_ptr<const scene::BakeScene> &source,
                  VkPhysicalDevice physical, VkDevice device, uint32_t family, const bake::Config &config) {
      VkPhysicalDeviceProperties limits;
      vkGetPhysicalDeviceProperties(physical, &limits);
      if (request->volumes.size() > imageCapacity(config.reflectionResolution,
              limits.limits.maxImageDimension2D, limits.limits.maxImageArrayLayers, limits.limits.maxStorageBufferRange))
        throw std::length_error("Authored reflection set exceeds GPU image or metadata limits");
      for (size_t index = 0; index < request->volumes.size(); ++index) {
        if (cancellation.stop_requested()) throw std::runtime_error("Reflection edit superseded");
        const auto &volume = request->volumes[index];
        Capture capture;
        capture.bounds.include(volume.minimum);
        capture.bounds.include(volume.maximum);
        capture.influence.include(volume.influenceMinimum);
        capture.influence.include(volume.influenceMaximum);
        capture.worldToLocal = volume.worldToLocal;
        capture.position = volume.capture;
        capture.priority = volume.priority;
        capture.blendDistance = volume.blendDistance < 0 ? .5f / source->sceneScale : volume.blendDistance;
        capture.resolution = config.reflectionResolution;
        int32_t oldIndex = -1;
        if (prior)
          for (size_t old = 0; old < prior->captures.size(); ++old)
            // Frozen scene and lighting configuration are unchanged here.
            // IDs and projection bounds do not affect world-space capture rays;
            // exact origin bits do, including signed zero in the shader seed.
            if (sameCaptureRays(prior->captures[old], capture)) {
              capture.radiance = prior->captures[old].radiance;
              oldIndex = int32_t(old);
              break;
            }
        reused.push_back(oldIndex);
        int32_t alias = -1;
        if (oldIndex < 0)
          for (size_t earlier = 0; earlier < captures.size(); ++earlier)
            if (sameCaptureRays(captures[earlier], capture)) {
              alias = int32_t(earlier);
              break;
            }
        aliases.push_back(alias);
        if (oldIndex < 0 && alias < 0) {
          capture.radiance.resize(6ull * capture.resolution * capture.resolution);
          rayOwners.push_back(uint32_t(index));
        }
        captures.push_back(std::move(capture));
      }
      // Plan reuse/aliases first, so a cold set reserves exactly its unique
      // captures instead of repeatedly reallocating an ever-growing ray list.
      const uint32_t texels = 6 * config.reflectionResolution * config.reflectionResolution;
      rays.reserve(size_t(rayOwners.size()) * texels);
      for (const auto owner : rayOwners) {
        if (cancellation.stop_requested()) throw std::runtime_error("Reflection edit superseded");
        appendRays(captures[owner], rays, cancellation.get_token());
      }
      rayCount = uint32_t(rays.size());
      if (prior && prior->transport) {
        // Immutable acceleration/textures are reused. Mutable dispatch slots
        // are accessed only after Authoring::selectFrame observed their fence.
        transport = prior->transport;
        prepared = prior->prepared;
        sceneReused = true;
      } else if (!rays.empty()) {
        bake::Options options;
        options.queueFamily = family;
        options.batchPoints = bake::Budget::maximumPoints;
        options.rayBudget = bake::Budget::maximumWork;
        options.initialBatchPoints = 1024;
        options.textureResolution = config.textureResolution;
        transport = std::make_shared<bake::Transport>();
        transport->initialize(physical, device, source, options);
      }
    }
    void finish(VkPhysicalDevice physical, VkDevice device) {
      // All submitted readbacks have retired before filtering starts. Future
      // edits reuse captures/filtered texels, never these transport work lists.
      // Release capacity before allocating filtered texels and image staging;
      // retain only the scalar count for diagnostics.
      std::vector<bake::Point>().swap(rays);
      std::vector<uint32_t>().swap(rayOwners);
      if (!captures.empty()) {
        const uint32_t size = captures.front().resolution;
        const uint32_t levels = std::bit_width(size), stride = levelOffset(size, levels);
        filtered.header.parameters = {uint32_t(captures.size()), size, levels, stride};
        filtered.texels.resize(captures.size() * stride);
        WorkerPool workers;
        for (size_t i = 0; i < captures.size(); ++i) {
          if (cancellation.stop_requested()) throw std::runtime_error("Reflection edit superseded");
          // Aliases always point backwards to an already completed capture.
          // Keep raw depth for this volume's independent projection confidence.
          if (aliases[i] >= 0)
            captures[i].radiance = captures[size_t(aliases[i])].radiance;
          filtered.header.probes[i] = projectionRecord(captures[i]);
          if (reused[i] >= 0) {
            const auto &priorTexels = previous->filtered.texels;
            std::copy_n(priorTexels.begin() + size_t(reused[i])*stride, stride,
                        filtered.texels.begin() + i*stride);
          } else if (aliases[i] >= 0) {
            std::copy_n(filtered.texels.begin() + size_t(aliases[i])*stride, stride,
                        filtered.texels.begin() + i*stride);
          } else {
            const auto one = probes::filter(std::span(captures).subspan(i, 1), 128,
                                             cancellation.get_token(), &workers);
            std::copy(one.texels.begin(), one.texels.end(), filtered.texels.begin() + i*stride);
          }
        }
      }
      std::vector<uint64_t> ids;
      ids.reserve(request->volumes.size());
      for (const auto &volume : request->volumes) ids.push_back(volume.id);
      identities = imageIdentities(filtered.header, ids);
      image = std::make_unique<ImageAsset>(physical, device, filtered, cancellation.get_token());
      if (automatic && !captures.empty() && automatic->specular) {
        const auto plan = compose(image->header(), automatic->specular->header());
        automaticCount = plan.automatic;
        covered = plan.covered;
        omitted = plan.omitted;
        if (automaticCount) {
          identities = composedIdentities(plan, std::span(identities).first(captures.size()));
          authoredImage = std::move(image);
          image = std::make_unique<ImageAsset>(physical, device, plan);
        }
      }
      previous.reset();
    }
  };
  Authoring::Authoring(VkPhysicalDevice physical, VkDevice device, pipeline::Service *pipelines,
                        bool hardware, uint32_t queueFamily)
      : m_physical(physical), m_device(device), m_pipelines(pipelines), m_hardware(hardware),
        m_queueFamily(queueFamily) {}
  Authoring::~Authoring() {
    if (m_job) m_job->cancellation.request_stop();
    for (const auto &job : m_retired) job->cancellation.request_stop();
  }
  void Authoring::retire() {
    if (!m_job) return;
    m_job->cancellation.request_stop();
    m_retired.push_back(std::move(m_job));
  }
  void Authoring::selectFrame(uint32_t slot) {
    m_slot = slot;
    auto &frame = m_frames.at(slot);
    if (frame.uploaded) {
      frame.uploaded->image->releaseUpload();
      frame.uploaded->authoredImage.reset();
      frame.uploaded->automatic.reset();
    }
    if (frame.work) {
      auto &job = *frame.work;
      if (frame.preparation) job.transport->releaseBuildInputs();
      if (frame.count) {
        std::vector<bake::Result> results(frame.count);
        job.transport->read(slot, results);
        const uint32_t texels = job.captures.front().resolution * job.captures.front().resolution * 6;
        for (uint32_t i = 0; i < frame.count; ++i) {
          const auto ray = frame.first + i;
          const auto owner = job.rayOwners.at(ray / texels);
          const auto &color = results[i].irradiance[0];
          for (float component : color)
            if (!std::isfinite(component) || component < 0) throw std::runtime_error("Invalid authored reflection readback");
          job.captures[owner].radiance[ray % texels] = color;
        }
        job.received += frame.count;
        if (const auto elapsed = job.transport->elapsed(slot))
          job.budget.observe(bake::Work::Reflection, frame.count, *elapsed, frame.budgetMilliseconds);
      }
    }
    frame = {};
    std::erase_if(m_retired, [](const auto &job) { return job->ticket->ready(); });
  }
  void Authoring::record(VkCommandBuffer command, RasterRenderer &resources, const bake::Config &config,
                          const std::shared_ptr<const bake::Volume> &automatic, bool fallback) {
    const auto source = resources.bakeScene();
    const auto request = resources.reflectionVolumes();
    if (source != m_source || !config.sameLighting(m_config)) {
      retire();
      m_active.reset();
      m_request.reset();
      m_source = source;
      m_config = config;
    }
    const bool useFallback = fallback && request && !request->volumes.empty();
    const auto fallbackVolume = useFallback ? automatic : nullptr;
    if (request != m_request || useFallback != m_fallback || fallbackVolume != m_automatic.lock()) {
      retire();
      m_request = request;
      m_fallback = useFallback;
      m_automatic = fallbackVolume;
      ++m_revision;
      m_status.clear();
      if (!request) m_active.reset();
    }
    // Keep the published override while an edit is preparing, but never sample
    // another map's override or trace before its world lighting has completed.
    m_frames[m_slot].sampled = m_active;
    if (!request || !source || !source->ready || !automatic ||
        source->contentHash != automatic->hash || !config.enabled) return;
    if (useFallback && (!automatic->specular || (automatic->reflectionHeader.parameters[0] &&
        automatic->reflectionHeader.parameters[1] != config.reflectionResolution))) return;
    if (!m_job && (!m_active || m_active->revision != m_revision) && m_status.empty()) {
      if (!m_hardware || !m_pipelines) { m_status = "Authored reflections require hardware ray queries"; return; }
      auto job = std::make_shared<Job>();
      job->request = request;
      job->revision = m_revision;
      job->automatic = fallbackVolume;
      const auto previous = m_active;
      job->previous = previous;
      const auto physical = m_physical;
      const auto device = m_device;
      const auto family = m_queueFamily;
      job->ticket = m_pipelines->prepare("authored-reflection-prepare-" + std::to_string(++generation),
          [job, previous, source, physical, device, family, config] {
        job->prepare(previous, source, physical, device, family, config);
      });
      m_job = std::move(job);
    }
    if (!m_job) { m_frames[m_slot].sampled = m_active; return; }
    auto &job = *m_job;
    if (!job.ticket->ready()) { m_frames[m_slot].sampled = m_active; return; }
    try {
      job.ticket->check();
      if (job.filtering) {
        if (job.authoredImage) {
          job.authoredImage->recordUpload(command);
          job.image->recordCompose(command, *job.authoredImage, *job.automatic->specular);
        } else {
          job.image->recordUpload(command);
        }
        m_frames[m_slot].uploaded = m_job;
        m_active = std::move(m_job);
        m_status.clear();
      } else if (job.transport && !job.prepared) {
        job.transport->recordPrepare(command, resources);
        job.prepared = true;
        m_frames[m_slot].work = m_job;
        m_frames[m_slot].preparation = true;
      } else if (job.next < job.rays.size()) {
        auto &frame = m_frames[m_slot];
        frame.first = job.next;
        frame.budgetMilliseconds = job.transport->timingSupported() ? config.gpuBudgetMs : 0;
        frame.count = job.budget.count(bake::Work::Reflection, config.batch, frame.budgetMilliseconds,
            config.samples, config.bounces, uint32_t(job.rays.size()) - job.next);
        frame.work = m_job;
        job.transport->recordRadiance(command, m_slot, std::span(job.rays).subspan(job.next, frame.count),
                                      config.bounces, config.samples);
        job.next += frame.count;
      } else if (job.received == job.rays.size()) {
        job.filtering = true;
        const auto owner = m_job;
        const auto physical = m_physical;
        const auto device = m_device;
        job.ticket = m_pipelines->prepare("authored-reflection-filter-" + std::to_string(++generation),
            [owner, physical, device] {
          owner->finish(physical, device);
        });
      }
    } catch (VkResult error) {
      m_status = "Authored reflection Vulkan failure " + std::to_string(int(error));
      std::fprintf(stderr, "%s\n", m_status.c_str());
      retire();
    } catch (const std::exception &error) {
      m_status = error.what();
      std::fprintf(stderr, "Authored reflections: %s\n", m_status.c_str());
      retire();
    }
    m_frames[m_slot].sampled = m_active;
  }
  Descriptors Authoring::descriptor(Descriptors automatic) const {
    return m_active ? m_active->image->descriptors() : automatic;
  }
  const Header *Authoring::header() const { return m_active ? &m_active->image->header() : nullptr; }
  const LayerIdentities *Authoring::identities() const { return m_active ? &m_active->identities : nullptr; }
  uint32_t Authoring::receiverLayer(uint64_t sceneId, uint64_t volumeId) const {
    if (!m_active || !volumeId || sceneId != m_active->request->sceneId) return 0;
    const auto count = m_active->image->header().parameters[0];
    for (uint32_t layer = 0; layer < count; ++layer)
      if (m_active->identities[layer] == volumeId) return layer + 1;
    return 0;
  }
  AuthoringStatistics Authoring::statistics() const {
    AuthoringStatistics result;
    result.requested = bool(m_request);
    result.failed = !m_status.empty();
    result.pending = bool(m_request) && (!m_active || m_active->revision != m_revision);
    result.active = bool(m_active);
    if (m_active) {
      result.publishedSceneId = m_active->request->sceneId;
      result.publishedRevision = m_active->request->revision;
      result.automatic = m_active->automaticCount;
      result.covered = m_active->covered;
      result.omitted = m_active->omitted;
    }
    const auto &job = m_job ? m_job : m_active;
    if (!job) return result;
    // Worker-owned vectors are read only after preparation has completed.
    result.revision = job->request->revision;
    if (job->ticket->ready()) {
      result.sceneReused = job->sceneReused;
      result.rays = job->rayCount;
      result.captureScratchBytes = job->rays.capacity() * sizeof(bake::Point) +
                                   job->rayOwners.capacity() * sizeof(uint32_t);
      result.completed = job->received;
      result.reused = uint32_t(std::count_if(job->reused.begin(), job->reused.end(), [](int32_t i) { return i >= 0; }));
      result.reused += uint32_t(std::count_if(job->aliases.begin(), job->aliases.end(), [](int32_t i) { return i >= 0; }));
    }
    return result;
  }
  AuthoringMemory Authoring::memory() const {
    AuthoringMemory result;
    std::unordered_set<const Job *> jobs;
    std::unordered_set<const bake::Transport *> scenes;
    auto count = [&](const std::shared_ptr<Job> &job) {
      if (!job || !jobs.insert(job.get()).second || !job->ticket->ready()) return;
      if (job->image) {
        result.buffers += job->image->bufferBytes();
        result.images += job->image->imageBytes();
      }
      if (job->authoredImage) {
        result.buffers += job->authoredImage->bufferBytes();
        result.images += job->authoredImage->imageBytes();
      }
      if (job->prepared && job->transport && scenes.insert(job->transport.get()).second) {
        result.buffers += job->transport->memoryBytes();
        result.images += job->transport->imageBytes();
        result.acceleration += job->transport->accelerationBytes();
      }
    };
    count(m_active); count(m_job);
    for (const auto &job : m_retired) count(job);
    for (const auto &frame : m_frames) { count(frame.work); count(frame.sampled); count(frame.uploaded); }
    return result;
  }
} // namespace remix::forward::probes
