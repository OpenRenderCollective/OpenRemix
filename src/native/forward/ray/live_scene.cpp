#include "forward/ray/live_scene.h"
#include "forward/options.h"
#include "forward/bake/transport.h"
#include "forward/ray/capture.h"
#include "platform/gpu_frames.h"
#include "raster/renderer.h"
#include <cmath>
#include <cstdio>
#include <optional>
#include <unordered_set>

namespace remix::forward::ray {
  struct LiveScene::Gpu {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t slot = 0;
    bool reflectionBatches = false;
    Statistics statistics;
    struct World {
      uint64_t key = 0;
      uint32_t resolution = 0;
      bool releasePending = false;
      bake::Transport transport;
    };
    struct Frame {
      std::shared_ptr<World> world;
      bool prepared = false;
    };
    std::array<Frame, gpuFrameCount> frames;
    std::shared_ptr<World> current;
    // Packing the whole scene is the costliest CPU work here. An identical
    // submission (draws, transforms, poses, emitters) reuses the last packing.
    uint64_t inputKey = 0;
    std::vector<bake::TransportLight> lights;
    uint64_t failedKey = 0;
    bool failed = false;
  };
  LiveScene::LiveScene() = default;
  LiveScene::~LiveScene() = default;
  void LiveScene::initialize(VkPhysicalDevice physical, VkDevice device, bool reflectionBatches) {
    auto next = std::make_unique<Gpu>();
    next->physical = physical;
    next->device = device;
    next->reflectionBatches = reflectionBatches;
    next->statistics.available = true;
    bake::Transport::preparePipelines(device);
    m_gpu = std::move(next);
  }
  bool LiveScene::supportsFragmentGI(VkPhysicalDevice physical) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    // Complete scene/material layouts plus the read-only transport scene set.
    const auto &limits = properties.limits;
    return limits.maxPerStageResources >= 33 &&
           limits.maxPerStageDescriptorUniformBuffers >= 4 && limits.maxDescriptorSetUniformBuffers >= 4 &&
           limits.maxPerStageDescriptorStorageBuffers >= 10 && limits.maxDescriptorSetStorageBuffers >= 12 &&
           limits.maxPerStageDescriptorSamplers >= 18 && limits.maxDescriptorSetSamplers >= 18 &&
           limits.maxPerStageDescriptorSampledImages >= 18 && limits.maxDescriptorSetSampledImages >= 18;
  }
  void LiveScene::selectFrame(uint32_t index) {
    auto &g = *m_gpu;
    g.slot = index;
    auto &frame = g.frames.at(index);
    if (frame.prepared && frame.world && frame.world->releasePending) {
      frame.world->transport.releaseBuildInputs();
      frame.world->releasePending = false;
    }
    frame.world.reset();
    frame.prepared = false;
  }
  void LiveScene::prepare(VkCommandBuffer command, RasterRenderer &resources, const Options &options,
                          const scene::DistantLight &liveSun, double seconds) {
    auto &g = *m_gpu;
    g.statistics.active = false;
    g.statistics.rays = 0;
    g.statistics.sceneBuilds = 0;
    const float distance = g.reflectionBatches ? options.rtReflectionDistance : options.rtGIDistance;
    const auto authored = resources.bakeScene();
    const auto &draws = resources.draws();
    const auto emitters = resources.sphereLights();
    uint64_t inputKey = scene::fastHash(&options.sceneScale, sizeof(options.sceneScale),
                                        reinterpret_cast<uintptr_t>(authored.get()));
    auto add = [&](const void *data, size_t bytes) { inputKey = scene::fastHash(data, bytes, inputKey); };
    for (const auto &draw : draws) {
      const uintptr_t owners[] = {reinterpret_cast<uintptr_t>(draw.surface.get()),
                                  reinterpret_cast<uintptr_t>(draw.material.get()),
                                  reinterpret_cast<uintptr_t>(draw.mesh.get())};
      add(owners, sizeof(owners));
      add(draw.model.data(), sizeof(draw.model));
      const uint64_t identity[] = {draw.instanceId, draw.instanceIndex, draw.surfaceIndex, draw.firstIndex,
                                   draw.indexCount, uint64_t(uint32_t(draw.vertexOffset)), draw.classification,
                                   uint64_t(draw.viewModel) | uint64_t(draw.reflectionOnly) << 1 |
                                       uint64_t(draw.rasterVisible) << 2 | uint64_t(draw.doubleSided) << 3};
      add(identity, sizeof(identity));
      if (draw.bones && !draw.bones->empty())
        add(draw.bones->data(), draw.bones->size() * sizeof(Matrix));
      // Packing reads time only through sprite-sheet animation: its frame index
      // (as in materialData) changes the packed scene, the clock alone does not.
      if (const auto &m = *draw.material; m.fps > 0 && uint32_t(m.rows) * m.columns > 1) {
        const uint32_t frame = uint32_t(std::fmod(seconds * m.fps, uint32_t(m.rows) * m.columns));
        add(&frame, sizeof(frame));
      }
    }
    for (const auto &light : emitters) {
      add(&light.id, sizeof(light.id));
      add(light.position, sizeof(light.position));
      add(&light.radius, sizeof(light.radius));
      add(light.radiance, sizeof(light.radiance));
      add(light.corners, sizeof(light.corners));
      add(light.direction, sizeof(light.direction));
      const float shape[] = {light.coneCos, light.softness, light.focus, float(light.style), float(light.triangle)};
      add(shape, sizeof(shape));
    }
    const bool complete = authored && authored->ready;
    const auto sun = liveSun.isDistant || !complete ? liveSun : authored->sun;
    const auto dome = resources.dome().enabled || !complete ? resources.dome() : authored->dome;
    std::optional<Capture> snapshot;
    std::vector<bake::TransportLight> lights;
    uint64_t sceneKey = 0;
    if (g.current && inputKey == g.inputKey) {
      lights = g.lights;
      sceneKey = g.current->key;
    } else {
      snapshot = capture(authored.get(), draws, emitters, liveSun, resources.dome(), options.sceneScale, seconds);
      lights = snapshot->inputs.lights;
      sceneKey = snapshot->key;
      g.lights = lights;
      g.inputKey = inputKey;
    }
    auto &frame = g.frames[g.slot];
    const bool changed =
        !g.current || g.current->key != sceneKey || g.current->resolution != options.rtTextureResolution;
    auto attemptKey = scene::fastHash(&distance, sizeof(distance), sceneKey);
    attemptKey =
        scene::fastHash(&options.rtTextureResolution, sizeof(options.rtTextureResolution), attemptKey);
    if (changed && !snapshot) {
      // A resolution change keeps the scene inputs; pack them again.
      snapshot = capture(authored.get(), draws, emitters, liveSun, resources.dome(), options.sceneScale, seconds);
    }
    if (changed && (!g.failed || g.failedKey != attemptKey)) {
      try {
        auto next = std::make_shared<Gpu::World>();
        next->key = snapshot->key;
        next->resolution = options.rtTextureResolution;
        bake::Options transport;
        transport.cpuReadback = false;
        transport.deviceLocalBatches = true;
        transport.batchPoints = g.reflectionBatches ? 16384u : 1u;
        transport.textureResolution = options.rtTextureResolution;
        transport.distance = distance;
        next->transport.initialize(g.physical, g.device, std::move(snapshot->source),
                                   std::move(snapshot->inputs), transport);
        g.current = std::move(next);
        frame.prepared = true;
        g.statistics.sceneBuilds = 1;
        g.failed = false;
      } catch (VkResult error) {
        if (error == VK_ERROR_DEVICE_LOST)
          throw;
        g.current.reset();
        g.failed = error != VK_NOT_READY;
        g.failedKey = attemptKey;
        std::snprintf(g.statistics.status, sizeof(g.statistics.status),
                      "Live ray scene unavailable (Vulkan %d); using SSR/probes", int(error));
      } catch (const std::exception &error) {
        g.current.reset();
        g.failed = true;
        g.failedKey = attemptKey;
        std::snprintf(g.statistics.status, sizeof(g.statistics.status), "Live ray scene: %.80s",
                      error.what());
      }
    }
    frame.world = g.current;
    // Recording can call the shared texture provider. Keep the owner before
    // recording; a recording failure must propagate so the caller abandons the
    // command buffer instead of submitting references to discarded resources.
    if (frame.prepared) {
      frame.world->transport.recordPrepare(command, resources);
      frame.world->releasePending = true;
    }
    if (frame.world)
      frame.world->transport.updateLighting(g.slot, lights, sun, dome, distance, float(seconds));
    g.statistics.active = bool(frame.world);
    if (frame.world)
      std::snprintf(g.statistics.status, sizeof(g.statistics.status), "Live ray scene active");
  }
  bake::Transport *LiveScene::transport() const {
    const auto &frame = m_gpu->frames[m_gpu->slot];
    return frame.world ? &frame.world->transport : nullptr;
  }
  const Statistics &LiveScene::statistics() const {
    return m_gpu->statistics;
  }
  LiveScene::Memory LiveScene::memory() const {
    const auto &g = *m_gpu;
    Memory result;
    std::unordered_set<const Gpu::World *> worlds;
    if (g.current)
      worlds.insert(g.current.get());
    for (const auto &frame : g.frames) {
      if (frame.world)
        worlds.insert(frame.world.get());
    }
    for (const auto *world : worlds) {
      const auto &transport = world->transport;
      result.images += transport.imageBytes();
      result.acceleration += transport.accelerationBytes();
      result.buffers += transport.memoryBytes() - transport.imageBytes() - transport.accelerationBytes();
    }
    return result;
  }
} // namespace remix::forward::ray
