#include "pathtrace/gpu.h"
#if REMIX_HAS_NUMOS
#include "sky/renderer.h"
#endif
#include "pathtrace/shader_code.h"
#include "runtime/pipelines.h"
#include "upscale/ngx.h"

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  void PathTracer::Gpu::swapFrame(FrameResources &frame) {
    using std::swap;
    swap(linearCapture, frame.linearCapture);
    swap(rayProfile.readback, frame.rayReadback);
    swap(rayProfile.pending, frame.rayPending);
    swap(pbrReuse.readback, frame.reuseReadback);
    swap(pbrReuse.pending, frame.reusePending);
    swap(set, frame.set);
    swap(timestamps, frame.timestamps);
    swap(compaction, frame.compaction);
    swap(compactionCapacity, frame.compactionCapacity);
    swap(timingPending, frame.timingPending);
    swap(traceState, frame.traceState);
    swap(table, frame.table);
    swap(tableVersion, frame.tableVersion);
    swap(retiredStates, frame.retiredStates);
    swap(tableUpload, frame.tableUpload);
    swap(tableShadow, frame.tableShadow);
    swap(uniform, frame.uniform);
    swap(scratch, frame.scratch);
    swap(topScratch, frame.topScratch);
    swap(lightNodes, frame.lightNodes);
    swap(lightEmitters, frame.lightEmitters);
    swap(torchNodes, frame.torchNodes);
    swap(torchEmitters, frame.torchEmitters);
    swap(emissiveNodes, frame.emissiveNodes);
    swap(emissiveEmitters, frame.emissiveEmitters);
    swap(uploads, frame.uploads);
    swap(staging, frame.staging);
    swap(primaryRecording, frame.primaryRecording);
    swap(retired, frame.retired);
    swap(compactPending, frame.compactPending);
    swap(retiredGeometry, frame.retiredGeometry);
    swap(retiredBuffers, frame.retiredBuffers);
    swap(sharc.timestamps, frame.sharcTimestamps);
    swap(sharc.readback, frame.sharcReadback);
    swap(sharc.pending, frame.sharcPending);
    swap(lightGuiding.readback, frame.guidingReadback);
    swap(lightGuiding.pending, frame.guidingPending);
    swap(lightBufferGeneration, frame.lightGeneration);
    swap(torchBufferGeneration, frame.torchGeneration);
    swap(emissiveBufferGeneration, frame.emissiveGeneration);
    swap(lightBufferInstance, frame.lightInstance);
    swap(torchBufferInstance, frame.torchInstance);
    swap(emissiveBufferInstance, frame.emissiveInstance);
    swap(retiredPreviousVertices, frame.retiredVertices);
    for (unsigned i = 0; i < 3; ++i)
      swap(tlas[i].instances, frame.instances[i]);
    swap(tlas[2].prefix, frame.combinedPrefix);
  }

  void PathTracer::Gpu::selectFrame(uint32_t slot) {
    if (slot != frameIndex) {
      swapFrame(frames[frameIndex]);
      frameIndex = slot;
      swapFrame(frames[frameIndex]);
    }
    if (!set) {
      const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = pool,
                                                 .descriptorSetCount = 1,
                                                 .pSetLayouts = &setLayout};
      check(vkAllocateDescriptorSets(device, &allocate, &set));
    }
    if (!timestamps && timestampBits && properties.limits.timestampComputeAndGraphics) {
      const VkQueryPoolCreateInfo query{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                        .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                        .queryCount = 12};
      check(vkCreateQueryPool(device, &query, nullptr, &timestamps));
    }
    if (sharc.hash && !sharc.readback) {
      sharc.readback = buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
      if (timestampBits) {
        const VkQueryPoolCreateInfo query{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                          .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                          .queryCount = 4};
        check(vkCreateQueryPool(device, &query, nullptr, &sharc.timestamps));
      }
    }
    if (lightGuiding.history[0] && !lightGuiding.readback)
      lightGuiding.readback = buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  }

  void PathTracer::selectFrame(uint32_t slot) {
    m_gpu->selectFrame(slot);
    m_gpu->postFallback->selectFrame(slot);
#if REMIX_HAS_NUMOS
    if (m_gpu->numosSky)
      m_gpu->numosSky->selectFrame(slot);
#endif
  }
  void PathTracer::setSky(const config::SkySettings &settings, const config::SkyControl &control,
                          const std::string &unavailable) {
    if (!(settings == m_gpu->skySettings)) {
      m_gpu->skyFailure.clear();
      m_gpu->skySettings = settings;
    }
    m_gpu->skyControl = control;
    m_gpu->skyUnavailable = unavailable;
  }
  config::SkyState PathTracer::skyState() const {
    std::lock_guard lock(m_gpu->skyStateMutex);
    return m_gpu->skyState;
  }
  PathTracer::SkyStatus PathTracer::skyStatus() const {
    SkyStatus status;
#if REMIX_HAS_NUMOS
    status.supported = m_gpu->skyUnavailable.empty();
    status.active = m_gpu->skyApplied;
    status.reason = !status.supported          ? "The device lacks " + m_gpu->skyUnavailable + "."
                    : !m_gpu->skyFailure.empty() ? "Stopped: " + m_gpu->skyFailure
                                                 : m_gpu->skyReason;
#else
    status.reason = "This build has no NUMOS sky.";
#endif
    return status;
  }
  std::shared_ptr<postfx::State> PathTracer::exposureState() const { return m_gpu->postFallback->state(); }
  VkExtent2D PathTracer::renderExtent() const { return m_gpu->extent; }
  VkImageView PathTracer::viewDepth() const {
    return m_gpu->denoiser ? m_gpu->denoiser->image(denoise::Depth).view : VK_NULL_HANDLE;
  }
  PathTracer::OverlayLighting PathTracer::overlayLighting() const {
    auto out = m_gpu->overlay;
    const auto &accel = m_gpu->tlas[m_gpu->topCombined ? 2 : 0].accel;
    out.scene = accel ? accel->handle : VK_NULL_HANDLE;
    return out;
  }
  void PathTracer::setPostProcessing(postfx::Renderer *renderer, bool retainExposure) {
    m_gpu->postProcessing = renderer ? renderer : m_gpu->postFallback.get();
    m_gpu->retainPostExposure = retainExposure;
  }
  bool PathTracer::adaptiveTerminationAvailable() const { return !m_gpu->rouletteFailed; }
  void PathTracer::resetPerformance() {
    m_gpu->performance = {.structSize = sizeof(remixapi_FramePerformanceEXT)};
    m_gpu->breakdown = {.structSize = sizeof(remixapi_FrameBreakdownEXT)};
    m_gpu->materialTicks = m_gpu->temporalTicks = m_gpu->geometryTicks = 0;
  }
  remixapi_FramePerformanceEXT PathTracer::performance() const {
    auto out = m_gpu->performance;
    out.geometryMilliseconds = tickMilliseconds(m_gpu->geometryTicks);
    return out;
  }
  remixapi_FrameBreakdownEXT PathTracer::breakdown() const {
    auto out = m_gpu->breakdown;
    out.materialMilliseconds = tickMilliseconds(m_gpu->materialTicks);
    out.temporalMilliseconds = tickMilliseconds(m_gpu->temporalTicks);
    return out;
  }
  uint64_t PathTracer::frameDigest() const { return m_gpu->digest; }

  PathTracer::Gpu::~Gpu() {
    destroyTarget();
    destroySharc();
    destroyPrimaryRecording(primaryRecording);
    for (auto &frame : frames) {
      destroyPrimaryRecording(frame.primaryRecording);
      if (frame.timestamps)
        vkDestroyQueryPool(device, frame.timestamps, nullptr);
      if (frame.compaction)
        vkDestroyQueryPool(device, frame.compaction, nullptr);
      if (frame.sharcTimestamps)
        vkDestroyQueryPool(device, frame.sharcTimestamps, nullptr);
      frame = {};
    }
    // Kept static tables refer to the views, and history states hold buffers.
    tables.reset();
    statics.clear();
    retiredStates.clear();
    retiredGeometry.clear();
    retiredBuffers.clear();
    deformViews.clear();
    meshes.clear();
    compactPending.clear();
    retired.clear();
    for (auto &top : tlas) {
      top.accel.reset();
      top.instances.reset();
    }
    arenas.clear();
    if (timestamps)
      vkDestroyQueryPool(device, timestamps, nullptr);
    if (compaction)
      vkDestroyQueryPool(device, compaction, nullptr);
    forgetLightingPipeline(singleTransparencyCompute);
    if (singleTransparencyCompute)
      pipeline::destroyPipeline(device, singleTransparencyCompute, nullptr);
    forgetLightingPipeline(sheetCompute);
    if (sheetCompute)
      pipeline::destroyPipeline(device, sheetCompute, nullptr);
    sheetResults.reset();
    forgetLightingPipeline(orca.sparse);
    if (orca.sparse)
      pipeline::destroyPipeline(device, orca.sparse, nullptr);
    orca.sparse = VK_NULL_HANDLE;
    forgetLightingPipeline(regir.presample);
    if (regir.presample)
      pipeline::destroyPipeline(device, regir.presample, nullptr);
    for (auto pipeline : {sparse.compact, sparse.arguments, volume.update}) {
      forgetLightingPipeline(pipeline);
      if (pipeline)
        pipeline::destroyPipeline(device, pipeline, nullptr);
    }
    forgetLightingPipeline(transparencyCompute);
    forgetLightingPipeline(compute);
    if (transparencyCompute)
      pipeline::destroyPipeline(device, transparencyCompute, nullptr);
    if (compute)
      pipeline::destroyPipeline(device, compute, nullptr);
    for (auto shader : shaders)
      if (shader)
        pipeline::destroyShaderModule(device, shader, nullptr);
    if (layout)
      pipeline::destroyPipelineLayout(device, layout, nullptr);
    if (pool)
      vkDestroyDescriptorPool(device, pool, nullptr);
    if (postSetLayout)
      pipeline::destroyDescriptorSetLayout(device, postSetLayout, nullptr);
    if (setLayout)
      pipeline::destroyDescriptorSetLayout(device, setLayout, nullptr);
  }

  std::unique_ptr<Image> PathTracer::Gpu::image(bool colorAttachment, VkFormat format, VkExtent2D size) {
    if (!size.width) size = extent;
    auto out = std::make_unique<Image>();
    out->device = device;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {size.width, size.height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       (colorAttachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u),
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateImage(device, &info, nullptr, &out->handle));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, out->handle, &requirements);
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(device, &allocate, nullptr, &out->memory));
    check(vkBindImageMemory(device, out->handle, out->memory, 0));
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = out->handle,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = info.format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(device, &view, nullptr, &out->view));
    return out;
  }

  void PathTracer::Gpu::destroyTarget() {
    for (auto &buffer : pbrReuse.history) buffer.reset();
    pbrReuse.valid = pbrReuse.active = pbrReuse.failed = false;
    if (upscaler) {
      reconstructedFrames += upscaler->statistics().frames;
      reconstructionResets += upscaler->statistics().resets;
    }
    upscaler.reset();
    reconstructionWarmFrames = 0;
    lightGuiding.valid = false;
    destroyPost();
    sharc.clear = true;
    destroyPrimary();
    history.clear();
    previousVertices.clear();
    pendingVertices.clear();
    retiredPreviousVertices.clear();
    if (skyFramebuffer)
      vkDestroyFramebuffer(device, skyFramebuffer, nullptr);
    if (skyPass)
      pipeline::destroyRenderPass(device, skyPass, nullptr);
    skyFramebuffer = VK_NULL_HANDLE;
    skyPass = VK_NULL_HANDLE;
    sky.reset();
    for (auto &pipeline : composite) {
      if (pipeline)
        pipeline::destroyPipeline(device, pipeline, nullptr);
      pipeline = VK_NULL_HANDLE;
    }
    world.reset();
    transmission.reset();
    extent = {};
    imagesInitialized = false;
    lastScene = {};
    stats.accumulationCount = 0;
  }

  PathTracer::PathTracer() = default;

  PathTracer::~PathTracer() = default;

  void PathTracer::initialize(VkPhysicalDevice physical, VkDevice device,
                              uint32_t queueFamily, bool preparing,
                              bool rasterPrimary, std::shared_ptr<postfx::State> exposureState) {
    m_gpu = std::make_unique<Gpu>();
    auto &g = *m_gpu;
    g.physical = physical;
    g.device = device;
    g.querySharcSupport();
    g.queueFamily = queueFamily;
    g.postFallback = std::make_unique<postfx::Renderer>();
    g.postFallback->initialize(physical, device, queueFamily, {1, 1}, {1, 1}, false, false, exposureState);
    g.postProcessing = g.postFallback.get();
    g.postSetLayout = postfx::compositeLayout(device);
    g.preparing = preparing;
    g.preparedRasterPrimary = rasterPrimary;
    g.postCapabilities = denoise::capabilities(physical);
    g.sigmaCapable = denoise::sigmaSupported(physical);
    g.localCapable[0] = denoise::localSupported(physical, false);
    g.localCapable[1] = denoise::localSupported(physical, true);
    vkGetPhysicalDeviceMemoryProperties(physical, &g.memory);
    vkGetPhysicalDeviceProperties(physical, &g.properties);
    g.opacityCache = scene::OpacityCache(g.properties.limits.subTexelPrecisionBits, std::getenv("REMIX_OPACITY_PROFILE") != nullptr);
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                              .pNext = &g.asProperties};
    vkGetPhysicalDeviceProperties2(physical, &properties);
#define LOAD(member, name)                                                                                   \
  g.member = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));                               \
  if (!g.member)                                                                                             \
  throw VK_ERROR_EXTENSION_NOT_PRESENT
    LOAD(createAs, vkCreateAccelerationStructureKHR);
    LOAD(destroyAs, vkDestroyAccelerationStructureKHR);
    LOAD(getAsAddress, vkGetAccelerationStructureDeviceAddressKHR);
    // Resolve 1.2 entry points only when tracing is selected, so loading the
    // raster runtime still works with a Vulkan 1.1 loader.
    LOAD(getBufferAddress, vkGetBufferDeviceAddress);
    LOAD(buildSizes, vkGetAccelerationStructureBuildSizesKHR);
    LOAD(buildAs, vkCmdBuildAccelerationStructuresKHR);
    LOAD(copyAs, vkCmdCopyAccelerationStructureKHR);
    LOAD(writeAsProperties, vkCmdWriteAccelerationStructuresPropertiesKHR);
#undef LOAD
    VkDescriptorSetLayoutBinding bindings[16] = {};
    for (unsigned i = 0; i < 16; ++i) {
      bindings[i] = {.binding = i,
                     .descriptorCount = i == 6 ? textureLimit : 1u,
                     .stageFlags =
                         VK_SHADER_STAGE_COMPUTE_BIT |
                         ((i == 2 || i == 3) ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                          : (i == 4 || i == 5 || i == 6) ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                         : 0u)};
      bindings[i].descriptorType = i < 2    ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                                   : i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                   : i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                   : i != 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo setLayout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = 16,
                                                 .pBindings = bindings};
    check(pipeline::createDescriptorSetLayout(device, &setLayout, nullptr,
                                              &g.setLayout));
    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 11},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, textureLimit}};
    for (auto &size : poolSizes)
      size.descriptorCount *= gpuFrameCount;
    VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = gpuFrameCount,
                                       .poolSizeCount = 5,
                                       .pPoolSizes = poolSizes};
    check(vkCreateDescriptorPool(device, &pool, nullptr, &g.pool));
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = g.pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &g.setLayout};
    check(vkAllocateDescriptorSets(device, &allocate, &g.set));
    const VkDescriptorSetLayout compositeSets[] = {g.setLayout, g.postSetLayout};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 2,
                                         .pSetLayouts = compositeSets,
                                         .pushConstantRangeCount = uint32_t(std::size(pushRanges)),
                                         .pPushConstantRanges = pushRanges};
    check(pipeline::createPipelineLayout(device, &layout, nullptr, &g.layout));
    const trace::ShaderCode codes[] = {trace::shader(trace::Stage::comp), trace::shader(trace::Stage::vert),
                                       trace::shader(trace::Stage::frag)};
    for (unsigned i = 0; i < 3; ++i) {
      VkShaderModuleCreateInfo info = {
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = codes[i].bytes, .pCode = codes[i].code};
      check(
          pipeline::createShaderModule(device, &info, nullptr, &g.shaders[i]));
    }
    // Small, and needed by the first deforming update (else full uploads until ready).
    g.createAssemblePipeline();
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    g.timestampBits = families.at(queueFamily).timestampValidBits;
    if (g.timestampBits && g.properties.limits.timestampComputeAndGraphics) {
      VkQueryPoolCreateInfo query = {
          .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
          .queryType = VK_QUERY_TYPE_TIMESTAMP,
          .queryCount = 12};
      check(vkCreateQueryPool(device, &query, nullptr, &g.timestamps));
    }
  }

  void PathTracer::Gpu::createRawPipelines() {
    const bool separateIndirect = indirectPlanned(lightingOptions);
    if (compute && transparencyCompute && singleTransparencyCompute && sheetCompute &&
        (!separateIndirect || indirectPass.pipeline))
      return;
    // One batch: the raw lighting passes compile concurrently.
    std::vector<LightingRequest> requests = {
        {lightingStage(trace::Stage::comp), layout, &compute, true, true, false, true, separateIndirect},
        {lightingStage(trace::Stage::transparency), layout, &transparencyCompute, false},
        {lightingStage(trace::Stage::singleTransparency), layout, &singleTransparencyCompute, false},
        {lightingStage(trace::Stage::sheetLighting), layout, &sheetCompute, false},
    };
    if (separateIndirect)
      requests.push_back({lightingStage(trace::Stage::indirect), layout, &indirectPass.pipeline, true});
    // The raw passes are what every frame falls back to.
    awaitLightingPipelines(requests);
  }

  void PathTracer::configureTarget(VkRenderPass pass, VkExtent2D extent, VkFormat depthFormat,
                                   VkImageView depthView) {
    auto &g = *m_gpu;
    g.displayPass = pass;
    g.displayExtent = extent;
    g.displayDepthFormat = depthFormat;
    g.displayDepth = depthView;
    if (!g.preparing)
      g.createTarget();
  }
  void PathTracer::setDisplayOutput(const DisplayOutput &output) { m_gpu->display = output; }
  void PathTracer::Gpu::createTarget() {
    if (!preparing)
      check(vkDeviceWaitIdle(device));
    destroyTarget();
    extent = displayExtent;
    for (auto backend = selectBackend(); backend != upscale::Backend::Native; backend = selectBackend()) {
      try {
        const bool dlss = backend != upscale::Backend::Fsr31;
        uint32_t rrPreset = 0;
        if (backend == upscale::Backend::DlssRr) {
          // The model's library first (NGX restarts for it while no DLSS
          // feature exists), then a preset of that library.
          rrModelNote.clear();
          ngx->useRayReconstructionModel(rrCnnRequested, rrModelNote);
          const bool cnn = ngx->capabilities().rrCnn;
          const bool own = cnn ? rrPresetRequested >= 1 && rrPresetRequested <= 3
                               : rrPresetRequested >= 4 && rrPresetRequested <= 6;
          // Defaults: C (CNN), E (transformer); D where the CNN model's library
          // stayed loaded for a transformer request (it has no E).
          if (own && rrCnnRequested == cnn)
            rrPreset = rrPresetRequested;
          else
            rrPreset = cnn ? (rrCnnRequested ? 3u : 4u) : 5u;
        }
        extent = dlss ? ngx->renderSize(displayExtent, fsrQuality, backend == upscale::Backend::DlssRr)
                      : upscale::renderSize(displayExtent, fsrQuality);
        auto next = std::make_unique<upscale::Renderer>();
        next->initialize(physical, device, queueFamily, extent, displayExtent, backend, ngx, fsrQuality, rrPreset);
        rrPresetCreated = rrPreset;
        upscaler = std::move(next);
        break;
      } catch (VkResult result) {
        if (preparing)
          throw;
        if (result == VK_ERROR_DEVICE_LOST) throw;
        fsrFailure = std::string(upscale::name(backend)) + " initialization failed (Vulkan " + std::to_string(result) + ")";
      } catch (const std::exception& e) {
        if (preparing)
          throw;
        fsrFailure = e.what();
      }
      failedBackends |= 1u << uint32_t(backend);
      ++reconstructionFailures;
      extent = displayExtent;
    }
    auto& g = *this;
    const auto pass = displayPass;
    const auto depthFormat = displayDepthFormat;
    const auto depthView = displayDepth;
    g.world = g.image();
    g.transmission = g.image(false, VK_FORMAT_R32G32B32A32_UINT);
    g.sky = g.image(true);
    g.createPrimary(depthFormat, depthView);
    if (g.upscaler) {
      std::string failure;
      try { g.upscaler->configureComposite(pass, g.primary[4]->view); }
      catch (VkResult result) {
        if (preparing)
          throw;
        if (result == VK_ERROR_DEVICE_LOST) throw;
        failure = "Reconstruction compositor initialization failed (Vulkan " + std::to_string(result) + ")";
      }
      if (!failure.empty()) {
        failedBackends |= 1u << uint32_t(upscaler->statistics().backend);
        ++reconstructionFailures;
        fsrFailure = failure;
        createTarget();
        return;
      }
    }
    VkAttachmentDescription attachments[] = {
        {.format = VK_FORMAT_R32G32B32A32_SFLOAT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.format = depthFormat,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1,
                                 .pColorAttachments = &color,
                                 .pDepthStencilAttachment = &depthRef};
    VkSubpassDependency dependencies[] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL,
         .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         .dstStageMask =
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
         .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask =
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
        {.srcSubpass = 0,
         .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask =
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT}};
    VkRenderPassCreateInfo rp{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                              .attachmentCount = 2,
                              .pAttachments = attachments,
                              .subpassCount = 1,
                              .pSubpasses = &subpass,
                              .dependencyCount = 2,
                              .pDependencies = dependencies};
    check(pipeline::createRenderPass(g.device, &rp, nullptr, &g.skyPass));
    VkImageView views[] = {g.sky->view, depthView};
    VkFramebufferCreateInfo fb{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                               .renderPass = g.skyPass,
                               .attachmentCount = 2,
                               .pAttachments = views,
                               .width = extent.width,
                               .height = extent.height,
                               .layers = 1};
    check(vkCreateFramebuffer(g.device, &fb, nullptr, &g.skyFramebuffer));
    VkPipelineShaderStageCreateInfo stages[] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                 .module = g.shaders[1],
                                                 .pName = "main"},
                                                {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                 .module = g.shaders[2],
                                                 .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType =
                                                      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .maxDepthBounds = 1};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 2,
                                                .pDynamicStates = states};
    for (unsigned i = 0; i < 2; ++i) {
      // First multiply the raster sky by colored transmission, then add diffuse radiance.
      // This keeps the visual sky out of lighting and supports tinted primary transmission.
      VkPipelineColorBlendAttachmentState attachment = {
          .blendEnable = VK_TRUE,
          .srcColorBlendFactor = i ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO,
          .dstColorBlendFactor = i ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_SRC_COLOR,
          .colorBlendOp = VK_BLEND_OP_ADD,
          .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
          .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
          .alphaBlendOp = VK_BLEND_OP_ADD,
          .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT};
      VkPipelineColorBlendStateCreateInfo blending = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
          .attachmentCount = 1,
          .pAttachments = &attachment};
      depth.depthWriteEnable = !i;
      VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                               .stageCount = 2,
                                               .pStages = stages,
                                               .pVertexInputState = &vertex,
                                               .pInputAssemblyState = &assembly,
                                               .pViewportState = &viewport,
                                               .pRasterizationState = &raster,
                                               .pMultisampleState = &samples,
                                               .pDepthStencilState = &depth,
                                               .pColorBlendState = &blending,
                                               .pDynamicState = &dynamic,
                                               .layout = g.layout,
                                               .renderPass = pass};
      check(pipeline::createGraphicsPipelines(
          g.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &g.composite[i]));
    }
  }

  void PathTracer::destroyTarget() {
    if (m_gpu)
      m_gpu->destroyTarget();
  }

  remixapi_TracingStatisticsEXT PathTracer::statistics() const {
    auto result = m_gpu->stats;
    result.redrawRequested = result.accumulationEnabled ||
        (m_gpu->upscaler && m_gpu->reconstructionWarmFrames < 128) ||
        (m_gpu->sharc.stats.active && m_gpu->sharc.hasReceivers && m_gpu->sharc.warmFrames < m_gpu->sharc.warmTarget);
    return result;
  }

} // namespace remix
