#include "forward/ray/live_scene.h"
#include "forward/options.h"
#include "forward/bake/transport.h"
#include "forward/ray/capture.h"
#include "platform/gpu_frames.h"
#include "raster/renderer.h"
#include <cstdio>
#include <unordered_set>

namespace remix::forward::ray {
  struct LiveScene::Gpu {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t slot = 0;
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
    uint64_t failedKey = 0;
    bool failed = false;
  };
  LiveScene::LiveScene() = default;
  LiveScene::~LiveScene() = default;
  void LiveScene::initialize(VkPhysicalDevice physical, VkDevice device) {
    auto next = std::make_unique<Gpu>();
    next->physical = physical;
    next->device = device;
    next->statistics.available = true;
    bake::Transport::preparePipelines(device);
    m_gpu = std::move(next);
  }
  bool LiveScene::supportsFragmentGI(VkPhysicalDevice physical) {
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    // Four ray-scene SSBOs plus the ordinary Forward+ fragment resources.
    return properties.limits.maxPerStageDescriptorStorageBuffers >= 8 &&
           properties.limits.maxDescriptorSetStorageBuffers >= 10 &&
           properties.limits.maxPerStageDescriptorSamplers >= 10 &&
           properties.limits.maxDescriptorSetSamplers >= 10;
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
                          double seconds) {
    auto &g = *m_gpu;
    g.statistics.active = false;
    g.statistics.rays = 0;
    g.statistics.sceneBuilds = 0;
    const float distance = options.rtReflections ? options.rtReflectionDistance : options.rtGIDistance;
    auto snapshot = capture(resources.bakeScene().get(), resources.draws(), resources.sphereLights(),
                            resources.light(), resources.dome(), options.sceneScale, seconds);
    const auto sun = snapshot.source->sun;
    const auto dome = snapshot.source->dome;
    auto lights = std::move(snapshot.inputs.lights);
    auto &frame = g.frames[g.slot];
    const bool changed =
        !g.current || g.current->key != snapshot.key || g.current->resolution != options.rtTextureResolution;
    auto attemptKey = scene::fastHash(&distance, sizeof(distance), snapshot.key);
    attemptKey =
        scene::fastHash(&options.rtTextureResolution, sizeof(options.rtTextureResolution), attemptKey);
    if (changed && (!g.failed || g.failedKey != attemptKey)) {
      try {
        auto next = std::make_shared<Gpu::World>();
        next->key = snapshot.key;
        next->resolution = options.rtTextureResolution;
        bake::Options transport;
        transport.cpuReadback = false;
        transport.deviceLocalBatches = true;
        transport.batchPoints = options.rtReflections ? 16384u : 1u;
        transport.textureResolution = options.rtTextureResolution;
        transport.distance = distance;
        next->transport.initialize(g.physical, g.device, std::move(snapshot.source),
                                   std::move(snapshot.inputs), transport);
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
      frame.world->transport.updateLighting(g.slot, lights, sun, dome, distance);
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
