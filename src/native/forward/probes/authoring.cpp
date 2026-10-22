#include "forward/probes/authoring.h"
#include "forward/bake/transport.h"
#include "forward/bake/budget.h"
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
    std::vector<uint32_t> destinations;
    std::shared_ptr<bake::Transport> transport;
    std::unique_ptr<ImageAsset> image;
    Data filtered;
    bake::Budget budget;
    std::vector<int32_t> reused;
    std::shared_ptr<Job> previous;
    uint32_t next = 0, received = 0;
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
        capture.influence = capture.bounds;
        capture.worldToLocal = volume.worldToLocal;
        capture.position = volume.capture;
        capture.priority = volume.priority;
        capture.blendDistance = .5f / source->sceneScale;
        capture.resolution = config.reflectionResolution;
        capture.radiance.resize(6ull * capture.resolution * capture.resolution);
        int32_t oldIndex = -1;
        if (prior)
          for (size_t old = 0; old < prior->request->volumes.size(); ++old)
            if (prior->request->volumes[old].id == volume.id &&
                prior->captures[old].position == capture.position &&
                prior->captures[old].resolution == capture.resolution) {
              capture.radiance = prior->captures[old].radiance;
              oldIndex = int32_t(old);
              break;
            }
        reused.push_back(oldIndex);
        if (oldIndex < 0) {
          const auto captureRays = probes::rays(capture);
          for (uint32_t i = 0; i < captureRays.size(); ++i) destinations.push_back(uint32_t(index * captureRays.size() + i));
          rays.insert(rays.end(), captureRays.begin(), captureRays.end());
        }
        captures.push_back(std::move(capture));
      }
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
      if (!captures.empty()) {
        const uint32_t size = captures.front().resolution;
        const uint32_t levels = std::bit_width(size), stride = levelOffset(size, levels);
        filtered.header.parameters = {uint32_t(captures.size()), size, levels, stride};
        filtered.texels.resize(captures.size() * stride);
        for (size_t i = 0; i < captures.size(); ++i) {
          if (cancellation.stop_requested()) throw std::runtime_error("Reflection edit superseded");
          filtered.header.probes[i] = projectionRecord(captures[i]);
          if (reused[i] >= 0) {
            const auto &priorTexels = previous->filtered.texels;
            std::copy_n(priorTexels.begin() + size_t(reused[i])*stride, stride,
                        filtered.texels.begin() + i*stride);
          } else {
            const auto one = probes::filter(std::span(captures).subspan(i, 1), 128,
                                             cancellation.get_token());
            std::copy(one.texels.begin(), one.texels.end(), filtered.texels.begin() + i*stride);
          }
        }
      }
      image = std::make_unique<ImageAsset>(physical, device, filtered, cancellation.get_token());
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
    if (frame.uploaded) frame.uploaded->image->releaseUpload();
    if (frame.work) {
      auto &job = *frame.work;
      if (frame.preparation) job.transport->releaseBuildInputs();
      if (frame.count) {
        std::vector<bake::Result> results(frame.count);
        job.transport->read(slot, results);
        const uint32_t texels = job.captures.front().resolution * job.captures.front().resolution * 6;
        for (uint32_t i = 0; i < frame.count; ++i) {
          const auto destination = job.destinations[frame.first+i];
          const auto &color = results[i].irradiance[0];
          for (float component : color)
            if (!std::isfinite(component) || component < 0) throw std::runtime_error("Invalid authored reflection readback");
          job.captures[destination / texels].radiance[destination % texels] = color;
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
                          uint64_t activeBakeHash) {
    const auto source = resources.bakeScene();
    const auto request = resources.reflectionVolumes();
    if (source != m_source || !config.sameLighting(m_config)) {
      retire();
      m_active.reset();
      m_request.reset();
      m_source = source;
      m_config = config;
    }
    if (request != m_request) {
      retire();
      m_request = request;
      m_status.clear();
      if (!request) m_active.reset();
    }
    // Keep the published override while an edit is preparing, but never sample
    // another map's override or trace before its world lighting has completed.
    if (!request || !source || !source->ready || !activeBakeHash ||
        source->contentHash != activeBakeHash || !config.enabled) return;
    if (!m_job && (!m_active || m_active->request != request) && m_status.empty()) {
      if (!m_hardware || !m_pipelines) { m_status = "Authored reflections require hardware ray queries"; return; }
      auto job = std::make_shared<Job>();
      job->request = request;
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
        job.image->recordUpload(command);
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
  const Header *Authoring::header() const { return m_active ? &m_active->filtered.header : nullptr; }
  AuthoringStatistics Authoring::statistics() const {
    const auto &job = m_job ? m_job : m_active;
    if (!job) return {};
    // Worker-owned vectors are read only after preparation has completed.
    AuthoringStatistics result;
    result.revision = job->request->revision;
    result.pending = bool(m_job);
    result.active = bool(m_active);
    if (job->ticket->ready()) {
      result.sceneReused = job->sceneReused;
      result.rays = uint32_t(job->rays.size());
      result.completed = job->received;
      result.reused = uint32_t(std::count_if(job->reused.begin(), job->reused.end(), [](int32_t i) { return i >= 0; }));
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
