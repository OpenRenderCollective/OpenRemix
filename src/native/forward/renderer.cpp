#include "forward/bake/transport.h"
#include "forward/gpu.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <stdexcept>

namespace remix::forward {
  Renderer::Renderer() = default;
  Renderer::~Renderer() = default;
  Renderer::Gpu::~Gpu() {
    waterPass.reset();
    baking.reset();
    localMaps.reset();
    reflections.reset();
    rayReflections.reset();
    liveScene.reset();
    for (auto &frame : frames) {
      if (frame.pool)
        vkDestroyDescriptorPool(device, frame.pool, nullptr);
      frame.commands.clear();
      frame.staticCommands.clear();
      frame.terrainCommands.clear();
      frame.waterCommands.clear();
    }
    geometries.clear();
    for (auto handle : {opaquePipeline, lightmapPipeline, shadowPipeline, localShadowPipeline,
                        clusterPipeline, compositePipeline, giOpaquePipeline, giLightmapPipeline,
                        terrainPipeline, terrainLightmapPipeline, giTerrainPipeline, giTerrainLightmapPipeline})
      if (handle)
        pipeline::destroyPipeline(device, handle, nullptr);
    for (auto handle : {layout, compositeLayout, giLayout})
      if (handle)
        pipeline::destroyPipelineLayout(device, handle, nullptr);
    for (auto handle : {sceneLayout, materialLayout, compositeSetLayout, giSceneLayout, postLayout})
      if (handle)
        pipeline::destroyDescriptorSetLayout(device, handle, nullptr);
    if (worldFramebuffer)
      vkDestroyFramebuffer(device, worldFramebuffer, nullptr);
    for (auto handle : shadowFramebuffers)
      if (handle)
        vkDestroyFramebuffer(device, handle, nullptr);
    for (auto handle : shadowViews)
      if (handle)
        vkDestroyImageView(device, handle, nullptr);
    for (auto handle : {worldPass, shadowPass})
      if (handle)
        pipeline::destroyRenderPass(device, handle, nullptr);
    for (auto handle : {sampler, shadowSampler, surfaceSampler, skySampler})
      if (handle)
        pipeline::destroySampler(device, handle, nullptr);
  }
  void Renderer::initialize(VkPhysicalDevice physical, VkDevice device, VkRenderPass output,
                            VkExtent2D extent, Options options, bool encodeOutput,
                            pipeline::Service *pipelines, bool hardwareRay,
                            std::shared_ptr<bake::Controller> baking, uint32_t queueFamily,
                            std::shared_ptr<postfx::State> exposure) {
    if (baking && !baking->compatible(physical, device, pipelines, hardwareRay, queueFamily))
      throw std::invalid_argument("Shared map bake belongs to another device or pipeline service");
    auto next = std::make_unique<Gpu>();
    next->physical = physical;
    next->device = device;
    next->extent = extent;
    next->options = options;
    next->encodeOutput = encodeOutput;
    next->giSupported = options.rtGI && hardwareRay && ray::LiveScene::supportsFragmentGI(physical);
    next->waterRaySupported = options.water && options.rtReflections && hardwareRay &&
                              water::Pass::supportsRayReflections(physical);
    next->statistics.rayGI.available = hardwareRay && ray::LiveScene::supportsFragmentGI(physical);
    next->createLayouts();
    next->createTargets();
    next->postProcessing = std::make_unique<postfx::Renderer>();
    next->postProcessing->initialize(physical, device, queueFamily, extent, extent, true, false,
                                     std::move(exposure), postfx::MeterMode::SceneRadiance);
    next->statistics.targetBytes += next->postProcessing->statistics().memoryBytes;
    next->localMaps = std::make_unique<shadows::Maps>();
    next->localMaps->initialize(physical, device, next->shadowPass, options.localShadowLights,
                                options.localShadowResolution);
    next->statistics.targetBytes += next->localMaps->memoryBytes();
    if (options.reflections()) {
      next->reflections = std::make_unique<Reflections>();
      next->reflections->initialize(physical, device, extent, next->samples, options,
                                    {next->depth->view, next->normalGuide->view, next->reflectanceGuide->view,
                                     next->resolved ? next->resolved->view : next->color->view,
                                     next->resolved ? next->resolved->handle : next->color->handle});
      next->statistics.targetBytes += next->reflections->targetBytes();
    }
    next->statistics.rayReflections.available = hardwareRay;
    if ((options.rtReflections && hardwareRay) || next->giSupported) {
      next->liveScene = std::make_unique<ray::LiveScene>();
      next->liveScene->initialize(physical, device);
    }
    if (options.rtReflections && hardwareRay) {
      next->rayReflections = std::make_unique<ray::Reflections>();
      next->rayReflections->initialize(physical, device, *next->liveScene);
    }
    next->createPipelines(output);
    if (options.water) {
      next->waterPass = std::make_unique<water::Pass>();
      next->waterPass->initialize(physical, device, extent, next->samples, next->sceneLayout,
                                  next->materialLayout,
                                  {next->color->view, next->depth->view,
                                   next->resolved ? next->resolved->view : VK_NULL_HANDLE}, next->waterRaySupported);
    }
    next->baking =
        baking ? std::move(baking)
               : std::make_shared<bake::Controller>(physical, device, pipelines, hardwareRay, queueFamily);
    next->baseTargetBytes = next->statistics.targetBytes;
    m_gpu = std::move(next);
  }
  std::shared_ptr<bake::Controller> Renderer::bakeController() const {
    return m_gpu ? m_gpu->baking : nullptr;
  }
  std::shared_ptr<postfx::State> Renderer::exposureState() const {
    return m_gpu ? m_gpu->postProcessing->state() : nullptr;
  }
  void Renderer::selectFrame(uint32_t slot) {
    auto &g = *m_gpu;
    g.slot = slot;
    g.postProcessing->selectFrame(slot);
    g.baking->selectFrame(slot);
    if (g.reflections)
      g.reflections->selectFrame(slot);
    if (g.liveScene)
      g.liveScene->selectFrame(slot);
    if (g.rayReflections)
      g.rayReflections->selectFrame(slot);
    auto &frame = g.frames.at(slot);
    frame.commands.clear();
    frame.staticCommands.clear();
    frame.terrainCommands.clear();
    frame.waterCommands.clear();
    frame.waterComposed = false;
    if (frame.pool)
      check(vkResetDescriptorPool(g.device, frame.pool, 0));
    std::erase_if(g.geometries, [](const auto &entry) { return entry.second->source.expired(); });
  }
  void Renderer::prepare(VkCommandBuffer command, RasterRenderer &resources, float seconds) {
    m_gpu->postProcessing->begin(command);
    gpu::GpuScope bakeTime(command, "Forward+ bake publication and transport");
    m_gpu->baking->record(command, resources, m_gpu->options.baking, m_gpu->options.lightCutoff);
    bakeTime.close();
    if (m_gpu->liveScene) {
      gpu::GpuScope liveTime(command, "Forward+ live ray scene");
      const double seconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - m_gpu->epoch).count();
      m_gpu->liveScene->prepare(command, resources, m_gpu->options, seconds);
    }
    auto &frame = m_gpu->frames[m_gpu->slot];
    frame.giScene = VK_NULL_HANDLE;
    frame.waterRayScene = VK_NULL_HANDLE;
    if (m_gpu->giSupported)
      m_gpu->statistics.rayGI = m_gpu->liveScene->statistics();
    if (m_gpu->giSupported && m_gpu->liveScene->transport()) {
      const auto &options = m_gpu->options;
      frame.giScene = m_gpu->liveScene->transport()->prepareFragment(
          m_gpu->slot, options.rtGISamples, options.rtGIBounces, options.rtGIDistance);
    }
    if (m_gpu->waterRaySupported && m_gpu->liveScene->transport()) {
      const auto &options = m_gpu->options;
      frame.waterRayScene = m_gpu->liveScene->transport()->prepareFragment(
          m_gpu->slot, 1, options.rtReflectionBounces, options.rtReflectionDistance,
          bake::Transport::FragmentUse::Reflection);
    }
    gpu::GpuScope uploadTime(command, "Forward+ frame uploads");
    m_gpu->prepareFrame(command, resources);
    uploadTime.close();
    m_gpu->recordWorld(command);
    if (m_gpu->reflections) {
      gpu::GpuScope reflectionTime(command, "Forward+ reflection resolve and composition", m_gpu->extent);
      m_gpu->reflections->record(
          command, m_gpu->slot, m_gpu->frames[m_gpu->slot].data, m_gpu->options,
          m_gpu->baking->reflectionDescriptor(),
          {frame.skyConstants->descriptor(), m_gpu->baking->descriptor(), frame.skyImage},
          m_gpu->rayReflections.get());
    }
    m_gpu->recordWater(command);
    m_gpu->recordExposure(command, resources, seconds);
    m_gpu->statistics.baking = m_gpu->baking->statistics();
    const auto &volume = m_gpu->baking->volume();
    m_gpu->statistics.bakeContent = volume ? volume->content : bake::ContentHashes{};
    m_gpu->statistics.reflectionProbes = volume ? volume->reflectionHeader : probes::Header{};
    m_gpu->statistics.reflectionSceneScale = volume ? volume->sceneScale : 1;
    const auto memory = m_gpu->baking->memory();
    m_gpu->statistics.bufferBytes += memory.buffers;
    m_gpu->statistics.targetBytes = m_gpu->baseTargetBytes + memory.images +
                                    (m_gpu->waterPass ? m_gpu->waterPass->targetBytes() : 0);
    m_gpu->statistics.accelerationBytes = memory.acceleration;
    if (m_gpu->liveScene) {
      const auto live = m_gpu->liveScene->memory();
      m_gpu->statistics.bufferBytes += live.buffers;
      m_gpu->statistics.targetBytes += live.images;
      m_gpu->statistics.accelerationBytes += live.acceleration;
    }
    if (m_gpu->rayReflections) {
      m_gpu->statistics.rayReflections = m_gpu->rayReflections->statistics();
      const auto live = m_gpu->rayReflections->memory();
      m_gpu->statistics.bufferBytes += live.buffers;
      m_gpu->statistics.targetBytes += live.images;
      m_gpu->statistics.accelerationBytes += live.acceleration;
    }
  }
  const Statistics &Renderer::statistics() const { return m_gpu->statistics; }
  const Options &Renderer::options() const { return m_gpu->options; }
  bool Renderer::owns(const scene::Draw &draw) const {
    return accepts(draw) || acceptsTerrainLayer(draw) || (m_gpu && m_gpu->waterPass && water::accepts(draw));
  }
  void Renderer::updateLighting(const Options &options) {
    if (!m_gpu->options.sameTargets(options))
      throw VK_ERROR_INITIALIZATION_FAILED;
    m_gpu->options = options;
  }
  void Renderer::Gpu::ensure(std::unique_ptr<Buffer> &buffer, VkDeviceSize bytes, VkBufferUsageFlags usage,
                             Buffer::Memory memory) {
    if (!buffer || buffer->capacity < bytes) {
      auto next = std::make_unique<Buffer>();
      next->create(physical, device, std::max<VkDeviceSize>(bytes, 256), usage, memory);
      buffer = std::move(next);
    }
  }
  VkDescriptorSet Renderer::Gpu::allocate(Frame &frame, VkDescriptorSetLayout setLayout) {
    const VkDescriptorSetAllocateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                           .descriptorPool = frame.pool,
                                           .descriptorSetCount = 1,
                                           .pSetLayouts = &setLayout};
    VkDescriptorSet set;
    check(vkAllocateDescriptorSets(device, &info, &set));
    return set;
  }
} // namespace remix::forward
