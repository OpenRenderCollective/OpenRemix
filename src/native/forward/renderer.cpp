#include "forward/bake/transport.h"
#include "forward/gpu.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <stdexcept>

namespace remix::forward {
  void Renderer::Gpu::publishStatistics() {
    statistics.baking = baking->statistics();
    const auto &volume = baking->volume();
    statistics.bakeContent = volume ? volume->content : bake::ContentHashes{};
    auto &irradiance = statistics.irradianceGrid;
    irradiance = {};
    if (volume) {
      const auto &header = volume->irradianceHeader;
      std::copy_n(header.dimensions.begin(), 3, irradiance.dimensions.begin());
      irradiance.slots = header.dimensions[3];
      irradiance.usable = volume->usableIrradianceProbes;
      irradiance.spacingMetres = header.originSpacing[3] * volume->sceneScale;
    }
    statistics.reflectionSceneScale = volume ? volume->sceneScale : 1;
    statistics.authoredReflections = authoredReflections->statistics();
    const auto authoredHeader = authoredReflections->header();
    statistics.reflectionProbes = authoredHeader ? *authoredHeader : volume ? volume->reflectionHeader : probes::Header{};
    const auto memory = baking->memory();
    const auto authoredMemory = authoredReflections->memory();
    statistics.bufferBytes += memory.buffers + authoredMemory.buffers;
    if (probeVisibility) statistics.bufferBytes += probeVisibility->bufferBytes();
    statistics.targetBytes = baseTargetBytes + memory.images + authoredMemory.images +
                             (waterPass ? waterPass->targetBytes() : 0) +
                             viewModelTarget->bytes() +
                             (viewModelSun ? viewModelSun->bytes() : 0) +
                             (viewModelLocalMaps ? viewModelLocalMaps->memoryBytes() : 0);
    statistics.accelerationBytes = memory.acceleration + authoredMemory.acceleration;
    if (liveScene) {
      const auto live = liveScene->memory();
      statistics.bufferBytes += live.buffers;
      statistics.targetBytes += live.images;
      statistics.accelerationBytes += live.acceleration;
    }
    if (rayReflections) {
      statistics.rayReflections = rayReflections->statistics();
      const auto live = rayReflections->memory();
      statistics.bufferBytes += live.buffers;
      statistics.targetBytes += live.images;
      statistics.accelerationBytes += live.acceleration;
    }
  }
  Renderer::Renderer() = default;
  Renderer::~Renderer() = default;
  Renderer::Gpu::~Gpu() {
    viewModelSun.reset();
    viewModelLocalMaps.reset();
    waterPass.reset();
    authoredReflections.reset();
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
      frame.viewModelCommands.clear();
      frame.viewModelCasters.clear();
    }
    geometries.clear();
    spareGeometry.clear();
    for (auto handle : {opaquePipeline, lightmapPipeline, partialLightmapPipeline, depthPipeline, depthLightmapPipeline,
                        shadowPipeline, localShadowPipeline,
                        clusterPipeline, compositePipeline, viewModelPipeline, giOpaquePipeline, giLightmapPipeline,
                        terrainPipeline, terrainLightmapPipeline, terrainPartialLightmapPipeline,
                        giTerrainPipeline, giTerrainLightmapPipeline})
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
                            std::shared_ptr<postfx::State> exposure, bool hardwareSrgb) {
    if (baking && !baking->compatible(physical, device, pipelines, hardwareRay, queueFamily))
      throw std::invalid_argument("Shared map bake belongs to another device or pipeline service");
    auto next = std::make_unique<Gpu>();
    next->physical = physical;
    next->device = device;
    next->extent = extent;
    next->fragmentStatistics = std::make_unique<FragmentStatistics>(physical, device);
    next->options = options;
    next->encodeOutput = encodeOutput;
    next->hardwareSrgb = hardwareSrgb;
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
    next->viewModelTarget = std::make_unique<viewmodel::Target>();
    next->viewModelTarget->initialize(physical, device, extent, next->samples);
    if (std::getenv("REMIX_DEBUG_PROBE_BATCH")) {
      next->probeVisibility = std::make_unique<probe_batch::Cache>(physical, device);
      next->probeVisibility->prepare();
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
    next->authoredReflections = std::make_unique<probes::Authoring>(physical, device, pipelines, hardwareRay, queueFamily);
    next->baseTargetBytes = next->statistics.targetBytes;
    m_gpu = std::move(next);
  }
  std::shared_ptr<bake::Controller> Renderer::bakeController() const {
    return m_gpu ? m_gpu->baking : nullptr;
  }
  std::shared_ptr<postfx::State> Renderer::exposureState() const {
    return m_gpu ? m_gpu->postProcessing->state() : nullptr;
  }
  void Renderer::activate(uint32_t slot) {
    selectFrame(slot);
    // Queue retirement precedes activation. Preparation workers must not
    // inspect a controller still used by the old renderer.
    m_gpu->publishStatistics();
  }
  void Renderer::selectFrame(uint32_t slot) {
    auto &g = *m_gpu;
    g.slot = slot;
    g.fragmentStatistics->selectFrame(slot);
    g.postProcessing->selectFrame(slot);
    g.baking->selectFrame(slot);
    g.authoredReflections->selectFrame(slot);
    if (g.reflections)
      g.reflections->selectFrame(slot);
    if (g.liveScene)
      g.liveScene->selectFrame(slot);
    if (g.rayReflections)
      g.rayReflections->selectFrame(slot);
    auto &frame = g.frames.at(slot);
    frame.probeVisibility.reset();
    frame.commands.clear();
    frame.staticCommands.clear();
    frame.terrainCommands.clear();
    frame.waterCommands.clear();
    frame.viewModelCommands.clear();
    frame.viewModelCasters.clear();
    frame.posedGeometry.resize(frame.posedUsed);
    frame.posedUsed = 0;
    frame.waterComposed = false;
    if (frame.pool)
      check(vkResetDescriptorPool(g.device, frame.pool, 0));
    g.retireGeometry();
  }
  void Renderer::prepare(VkCommandBuffer command, RasterRenderer &resources, float seconds) {
    Statistics::CpuRecording cpu;
    auto began = std::chrono::steady_clock::now();
    auto step = [&](double &milliseconds) {
      const auto now = std::chrono::steady_clock::now();
      milliseconds = std::chrono::duration<double, std::milli>(now - began).count();
      began = now;
    };
    m_gpu->postProcessing->begin(command);
    gpu::GpuScope bakeTime(command, "Forward+ bake publication and transport");
    m_gpu->baking->record(command, resources, m_gpu->options.bakeConfiguration(m_gpu->giSupported),
                         m_gpu->options.lightCutoff);
    const auto &baked = m_gpu->baking->volume();
    m_gpu->authoredReflections->record(command, resources,
        m_gpu->options.bakeConfiguration(m_gpu->giSupported), baked, m_gpu->options.authoredReflectionFallback);
    bakeTime.close();
    step(cpu.bake);
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
    step(cpu.liveScene);
    gpu::GpuScope uploadTime(command, "Forward+ frame uploads");
    m_gpu->prepareFrame(command, resources);
    cpu.preparation = m_gpu->statistics.cpuRecording.preparation;
    uploadTime.close();
    step(cpu.frame);
    m_gpu->recordWorld(command);
    step(cpu.world);
    if (m_gpu->reflections) {
      gpu::GpuScope reflectionTime(command, "Forward+ reflection resolve and composition", m_gpu->extent);
      m_gpu->reflections->record(
          command, m_gpu->slot, m_gpu->frames[m_gpu->slot].data, m_gpu->options,
          m_gpu->reflectionDescriptor(),
          {frame.skyConstants->descriptor(), m_gpu->baking->descriptor(), frame.skyImage},
          m_gpu->rayReflections.get());
    }
    step(cpu.reflections);
    m_gpu->recordWater(command);
    m_gpu->recordViewModels(command);
    m_gpu->recordExposure(command, resources, seconds);
    step(cpu.waterExposure);
    m_gpu->publishStatistics();
    step(cpu.publication);
    m_gpu->statistics.cpuRecording = cpu;
  }
  const Statistics &Renderer::statistics() const { return m_gpu->statistics; }
  const Options &Renderer::options() const { return m_gpu->options; }
  bool Renderer::owns(const scene::Draw &draw) const {
    return accepts(draw) || acceptsViewModel(draw) || acceptsSurfaceLayer(draw) ||
           (m_gpu && m_gpu->waterPass && water::accepts(draw));
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
