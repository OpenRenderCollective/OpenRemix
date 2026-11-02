#include "forward/gpu.h"
#include "forward/bake/transport_memory.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace remix::forward {
  void Renderer::Gpu::publishStatistics() {
    statistics.debugView = options.debugView;
    statistics.bicubicLightmaps = options.bicubicLightmaps;
    statistics.baking = baking->statistics();
    const auto &volume = baking->volume();
    statistics.bakeContent = baking->content();
    auto &irradiance = statistics.irradianceGrid;
    irradiance = {};
    if (volume) {
      const auto &header = volume->irradianceHeader;
      std::copy_n(header.dimensions.begin(), 3, irradiance.dimensions.begin());
      irradiance.slots = header.dimensions[3];
      irradiance.usable = volume->usableIrradianceProbes;
      irradiance.spacingMetres = header.originSpacing[3] * volume->sceneScale;
      irradiance.finestSpacingMetres = volume->finestIrradianceSpacing * volume->sceneScale;
      irradiance.coarseRecords = volume->coarseIrradianceProbes;
      irradiance.patchCounts = volume->irradiancePatchCounts;
    }
    statistics.reflectionSceneScale = volume ? volume->sceneScale
        : standaloneReflectionsUsed && !options.baking.enabled ? standaloneReflections->sceneScale()
                                                                              : baking->reflectionSceneScale();
    statistics.authoredReflections = authoredReflections->statistics();
    const auto header = reflectionHeader();
    statistics.reflectionProbes = header ? *header : probes::Header{};
    const auto identities = reflectionIdentities();
    statistics.reflectionIdentities = identities ? *identities : probes::LayerIdentities{};
    bake::TransportMemory transportMemory;
    const auto memory = baking->memory(&transportMemory);
    const auto authoredMemory = authoredReflections->memory(&transportMemory);
    const auto standaloneMemory = standaloneReflectionsUsed ? standaloneReflections->memory(&transportMemory)
                                                          : probes::StandaloneCapture::Memory{};
    auto selectedMemory = localSelections.memory();
    if (localSelectionPreparation) {
      const auto prepared = localSelectionPreparation->memory();
      selectedMemory[0] += prepared[0]; selectedMemory[1] += prepared[1];
    }
    statistics.bufferBytes += memory.buffers + authoredMemory.buffers + standaloneMemory.buffers + selectedMemory[0];
    auto &visibility = statistics.probeVisibility;
    visibility = {};
    if (probeVisibility) {
      visibility.residentBytes = probeVisibility->bufferBytes();
      visibility.peakResidentBytes = probeVisibility->peakBufferBytes();
      visibility.recordedGathers = probeVisibility->recordedGathers();
      visibility.completedReuses = probeVisibility->completedReuses();
      statistics.bufferBytes += visibility.residentBytes;
      if (const auto &field = frames[slot].probeVisibility) {
        visibility.lights = uint32_t(field->lights().size());
        visibility.groups = field->groups();
        visibility.fieldBytes = field->bufferBytes();
        // Diagnostic identity of the ordered captured-light slots; never used
        // as a cache key in place of the exact source/ID comparison.
        visibility.selectionHash = 14695981039346656037ull;
        for (uint32_t light : field->lights()) {
          visibility.selectionHash ^= light;
          visibility.selectionHash *= 1099511628211ull;
        }
      }
    }
    statistics.targetBytes = baseTargetBytes + memory.images + authoredMemory.images + standaloneMemory.images + selectedMemory[1] +
                             (waterPass ? waterPass->targetBytes() : 0) +
                             viewModelTarget->bytes() +
                             (viewModelSun ? viewModelSun->bytes() : 0) +
                             (viewModelLocalMaps ? viewModelLocalMaps->memoryBytes() : 0);
    statistics.accelerationBytes = memory.acceleration + authoredMemory.acceleration + standaloneMemory.acceleration;
    statistics.rayGI = rayLighting.diffuseStatistics();
    statistics.rayReflections = rayLighting.reflectionStatistics();
    const auto rayMemory = rayLighting.memory();
    statistics.bufferBytes += rayMemory.buffers;
    statistics.targetBytes += rayMemory.images;
    statistics.accelerationBytes += rayMemory.acceleration;
  }
  Renderer::Renderer() = default;
  Renderer::~Renderer() = default;
  Renderer::Gpu::~Gpu() {
    localSelectionPreparation.reset();
    viewModelSun.reset();
    viewModelLocalMaps.reset();
    waterPass.reset();
    authoredReflections.reset();
    baking.reset();
    localMaps.reset();
    reflections.reset();
    rayLighting.reset();
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
    for (auto handle : {opaquePipeline, lightmapPipeline, partialLightmapPipeline, unmappedLightmapPipeline,
                        depthPipeline, depthLightmapPipeline, depthBatchPipeline, zeroDiffusePipeline,
                        shadowPipeline, localShadowPipeline,
                        clusterPipeline, compositePipeline, viewModelPipeline, viewModelEarlyPipeline, viewModelDepthPipeline,
                        giOpaquePipeline, giLightmapPipeline,
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
                            std::shared_ptr<postfx::State> exposure, bool hardwareSrgb,
                            std::shared_ptr<probes::Authoring> authoredReflections,
                            std::shared_ptr<probes::StandaloneCapture> standaloneReflections) {
    if (baking && !baking->compatible(physical, device, pipelines, hardwareRay, queueFamily))
      throw std::invalid_argument("Shared map bake belongs to another device or pipeline service");
    if (authoredReflections && !authoredReflections->compatible(physical, device, pipelines, hardwareRay, queueFamily))
      throw std::invalid_argument("Shared reflection captures belong to another device or pipeline service");
    if (standaloneReflections && !standaloneReflections->compatible(physical, device, pipelines, hardwareRay, queueFamily))
      throw std::invalid_argument("Shared standalone captures belong to another device or pipeline service");
    auto next = std::make_unique<Gpu>();
    next->physical = physical;
    next->device = device;
    if (pipelines) next->localSelectionPreparation = std::make_unique<probes::LocalSelectionPreparation>(physical, device, *pipelines);
    next->extent = extent;
    next->fragmentStatistics = std::make_unique<FragmentStatistics>(physical, device);
    next->options = options;
    next->encodeOutput = encodeOutput;
    next->hardwareSrgb = hardwareSrgb;
    next->capabilities = ray::Provider::capabilities(physical, hardwareRay);
    next->techniquePlan = techniques::resolveResources(options, next->capabilities);
    next->rayLighting.initialize(physical, device, next->techniquePlan, next->capabilities);
    next->statistics.rayGI = next->rayLighting.diffuseStatistics();
    next->statistics.rayReflections = next->rayLighting.reflectionStatistics();
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
    if (next->techniquePlan.reflectionResolve) {
      next->reflections = std::make_unique<Reflections>();
      next->reflections->initialize(physical, device, extent, next->samples, options,
                                    {next->depth->view, next->normalGuide->view, next->reflectanceGuide->view,
                                     next->resolved ? next->resolved->view : next->color->view,
                                     next->resolved ? next->resolved->handle : next->color->handle});
      next->statistics.targetBytes += next->reflections->targetBytes();
    }
    next->viewModelTarget = std::make_unique<viewmodel::Target>();
    next->viewModelTarget->initialize(physical, device, extent, next->samples);
    // Gather immutable selected-light masks once, then reuse vector lookups on
    // probe-lit receivers. Unsupported/budget-limited fields retain sparse reads.
    if (!std::getenv("REMIX_DEBUG_DISABLE_PROBE_BATCH")) {
      next->probeVisibility = std::make_unique<probe_batch::Cache>(physical, device);
      next->probeVisibility->prepare();
    }
    next->createPipelines(output);
    if (options.water) {
      next->waterPass = std::make_unique<water::Pass>();
      next->waterPass->initialize(physical, device, extent, next->samples, next->sceneLayout,
                                  next->materialLayout,
                                  {next->color->view, next->depth->view,
                                   next->resolved ? next->resolved->view : VK_NULL_HANDLE}, next->techniquePlan.rayWater);
    }
    next->baking =
        baking ? std::move(baking)
               : std::make_shared<bake::Controller>(physical, device, pipelines, hardwareRay, queueFamily);
    next->standaloneReflectionsUsed = standaloneReflections && standaloneReflections->wasUsed();
    next->standaloneReflections = standaloneReflections ? std::move(standaloneReflections)
        : std::make_shared<probes::StandaloneCapture>(physical, device, pipelines, hardwareRay, queueFamily);
    next->authoredReflections = authoredReflections ? std::move(authoredReflections)
        : std::make_shared<probes::Authoring>(physical, device, pipelines, hardwareRay, queueFamily);
    next->baseTargetBytes = next->statistics.targetBytes;
    m_gpu = std::move(next);
  }
  std::shared_ptr<bake::Controller> Renderer::bakeController() const {
    return m_gpu ? m_gpu->baking : nullptr;
  }
  std::shared_ptr<postfx::State> Renderer::exposureState() const {
    return m_gpu ? m_gpu->postProcessing->state() : nullptr;
  }
  std::shared_ptr<probes::Authoring> Renderer::reflectionAuthoring() const {
    return m_gpu ? m_gpu->authoredReflections : nullptr;
  }
  std::shared_ptr<probes::StandaloneCapture> Renderer::standaloneCaptures() const {
    return m_gpu ? m_gpu->standaloneReflections : nullptr;
  }
  void Renderer::activate(uint32_t slot) {
    // The old renderer may first use the shared controller after the replacement
    // worker's snapshot. Recheck only after queue-idle activation, before fences
    // and retained jobs are advanced by the new renderer.
    m_gpu->standaloneReflectionsUsed |= m_gpu->standaloneReflections->wasUsed();
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
    if (g.standaloneReflectionsUsed) g.standaloneReflections->selectFrame(slot);
    g.localSelections.selectFrame(slot);
    if (g.reflections)
      g.reflections->selectFrame(slot);
    g.rayLighting.selectFrame(slot);
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
  void Renderer::prepare(VkCommandBuffer command, RasterRenderer &resources, float seconds, double animationSeconds) {
    if (!std::isfinite(animationSeconds))
      throw std::invalid_argument("Invalid animation time");
    if (animationSeconds < 0)
      animationSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_gpu->epoch).count();
    Statistics::CpuRecording cpu;
    auto began = std::chrono::steady_clock::now();
    auto step = [&](double &milliseconds) {
      const auto now = std::chrono::steady_clock::now();
      milliseconds = std::chrono::duration<double, std::milli>(now - began).count();
      began = now;
    };
    m_gpu->postProcessing->begin(command);
    gpu::GpuScope bakeTime(command, "Forward+ bake publication and transport");
    const auto lightingPlan = m_gpu->options.lightingPlan(m_gpu->techniquePlan.rayDiffuse());
    m_gpu->baking->record(command, resources, lightingPlan.baking,
                         m_gpu->options.lightCutoff);
    if (!lightingPlan.baking.enabled || m_gpu->standaloneReflectionsUsed) {
      if (!m_gpu->standaloneReflectionsUsed) {
        m_gpu->standaloneReflectionsUsed = true;
        m_gpu->standaloneReflections->selectFrame(m_gpu->slot);
      }
      auto captureConfig = m_gpu->options.captureConfiguration();
      captureConfig.enabled = captureConfig.enabled && !lightingPlan.baking.enabled;
      m_gpu->standaloneReflections->record(command, resources, captureConfig,
          m_gpu->options.baking.reflectionProbes, m_gpu->options.baking.reflectionDetailTolerance);
    }
    auto captures = m_gpu->reflectionSource();
    // Publish automatic images independently, but preserve the baked profile's
    // transport priority: authored captures must not add a second work budget
    // while diffuse/style transport is still running.
    const bool capturesReady = lightingPlan.baking.enabled
        ? m_gpu->baking->statistics().state == REMIXAPI_BAKE_READY_EXT
        : !m_gpu->standaloneReflections->pending();
    if (!lightingPlan.baking.enabled) {
      const auto source = resources.bakeScene();
      captures.sceneHash = source && source->ready ? source->contentHash : 0;
    }
    m_gpu->authoredReflections->record(command, resources,
        m_gpu->options.captureConfiguration(), captures, m_gpu->options.authoredReflectionFallback, capturesReady);
    m_gpu->prepareLocalReflections(command, resources);
    bakeTime.close();
    step(cpu.bake);
    ray::Provider::FrameInputs rayInputs;
    if (m_gpu->techniquePlan.rayScene)
      rayInputs = m_gpu->rayLighting.prepare(command, resources, m_gpu->options,
                                              animationSeconds, m_gpu->slot);
    auto &frame = m_gpu->frames[m_gpu->slot];
    frame.giScene = rayInputs.diffuse;
    frame.waterRayScene = rayInputs.water;
    step(cpu.liveScene);
    gpu::GpuScope uploadTime(command, "Forward+ frame uploads");
    m_gpu->prepareFrame(command, resources, animationSeconds);
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
          m_gpu->rayLighting.reflections());
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
  void Renderer::setDisplayOutput(const scene::DisplayOutput &output) { m_gpu->displayOutput = output; }
  bool Renderer::owns(const scene::Draw &draw) const {
    return accepts(draw) || acceptsViewModel(draw) || acceptsSurfaceLayer(draw) ||
           (m_gpu && m_gpu->waterPass && water::accepts(draw));
  }
  void Renderer::updateLighting(const Options &options) {
    if (!m_gpu->options.sameTargets(options))
      throw VK_ERROR_INITIALIZATION_FAILED;
    m_gpu->options = options;
    m_gpu->techniquePlan = techniques::resolveResources(options, m_gpu->capabilities);
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
