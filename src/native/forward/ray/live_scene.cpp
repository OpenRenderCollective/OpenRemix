#include "forward/ray/live_scene.h"
#include "forward/options.h"
#include "forward/bake/transport.h"
#include "forward/ray/capture.h"
#include "platform/gpu_frames.h"
#include "raster/renderer.h"
#include <algorithm>
#include <cstdio>
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
    // The last capture, without its packed inputs: its signature and key, and
    // its source, which keeps the surfaces the signature names alive.
    Capture captured;
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
                          double seconds) {
    auto &g = *m_gpu;
    g.statistics.active = false;
    g.statistics.rays = 0;
    g.statistics.sceneBuilds = 0;
    // One range per submission. A ray that ends at the range limit reads as a
    // miss and takes the environment, which is wrong for the cascades: in a room
    // larger than the range, every long ray would bring the sky indoors. They
    // trace to the far end of any scene instead, and reflection batches reject
    // hits beyond their own range.
    constexpr float cascadeRange = 10000; // metres, the largest configurable range
    const bool reflections = g.reflectionBatches && options.rtReflections;
    float distance = reflections ? options.rtReflectionDistance : options.rtGIDistance;
    if (options.splitCascades) distance = std::max(distance, cascadeRange);
    std::vector<scene::SphereLight> lightScratch;
    const auto lightsView = resources.sphereLightView(lightScratch);
    // Geometry is only packed again when something it depends on changed since
    // the capture the current world was built from.
    const bool reusable = g.current && g.captured.source && g.captured.key == g.current->key &&
                          g.current->resolution == options.rtTextureResolution;
    auto snapshot = capture(resources.bakeScene().get(), resources.draws(), lightsView,
                            resources.light(), resources.dome(), options.sceneScale, seconds,
                            reusable ? &g.captured : nullptr);
    g.captured.source = snapshot.source;
    g.captured.key = snapshot.key;
    g.captured.signature = snapshot.signature;
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
        // Each batch is three dispatches with barriers between them. Small batches leave the GPU
        // mostly idle: a 1440p frame was hundreds of them. 131072 rays is the largest batch
        // the ray budget allows at eight bounces.
        transport.batchPoints = g.reflectionBatches ? 131072u : 1u;
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
