#include "denoise/gpu.h"
#include "denoise/shaders/embedded.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <cstring>

namespace remix::denoise {
  remixapi_DenoisingCapabilitiesEXT capabilities(VkPhysicalDevice physical) {
    remixapi_DenoisingCapabilitiesEXT result{.structSize = sizeof(result)};
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(physical, &f);
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(physical, &p);
    std::string reason;
    auto require = [&](bool ok, const char *name) {
      if (!ok) {
        if (!reason.empty())
          reason += ", ";
        reason += name;
      }
    };
    require(p.apiVersion >= VK_API_VERSION_1_2, "Vulkan 1.2");
    require(f.shaderStorageImageExtendedFormats, "extended storage-image formats");
    // The temporal set is used alongside the tracer set (ten storage images,
    // 4096 textures, two acceleration structures, a storage buffer and a UBO).
    require(p.limits.maxPerStageDescriptorStorageImages >= BaseImageCount + 37 &&
                p.limits.maxDescriptorSetStorageImages >= BaseImageCount + 37,
            "temporal storage-image descriptor limit");
    require(p.limits.maxPerStageResources >= 4096 + BaseImageCount + 42, "temporal per-stage resource limit");
    const VkFormat formats[] = {VK_FORMAT_R16G16B16A16_SFLOAT,
                                VK_FORMAT_R32G32B32A32_SFLOAT,
                                VK_FORMAT_R32_SFLOAT,
                                VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                VK_FORMAT_R8_UNORM,
                                VK_FORMAT_R8_UINT,
                                VK_FORMAT_R16_UINT,
                                VK_FORMAT_R16_SFLOAT};
    for (auto format : formats) {
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
      require((fp.optimalTilingFeatures &
               (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) ==
                  (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
              "NRD sampled/storage format");
    }
    for (auto format : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                        VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8_UNORM}) {
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
      require(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
              "temporal linear texture filtering");
    }
    result.taaSupported = reason.empty();
    require(f.shaderStorageImageReadWithoutFormat && f.shaderStorageImageWriteWithoutFormat,
            "storage-image access without format");
#ifndef REMIX_HAS_NRD
    require(false, "NRD disabled at build time");
#endif
    result.reblurShSupported = reason.empty();
    std::strncpy(result.unavailableReason, reason.c_str(), sizeof(result.unavailableReason) - 1);
    return result;
  }
  bool sigmaSupported(VkPhysicalDevice physical) {
    if (!capabilities(physical).reblurShSupported)
      return false;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(physical, &p);
    if (p.limits.maxPerStageDescriptorStorageImages < SigmaImageCount + 35 ||
        p.limits.maxDescriptorSetStorageImages < SigmaImageCount + 35 ||
        p.limits.maxPerStageResources < 4096 + SigmaImageCount + 40)
      return false;
    for (auto format : {VK_FORMAT_R32_UINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
      VkFormatFeatureFlags required =
          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      if (format != VK_FORMAT_R32_UINT)
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      if ((fp.optimalTilingFeatures & required) != required)
        return false;
    }
    return true;
  }
  bool localSupported(VkPhysicalDevice physical, bool sigma, bool split) {
    if (!capabilities(physical).reblurShSupported)
      return false;
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(physical, &p);
    const uint32_t count = BaseImageCount + (sigma ? 3 : 0) + (split ? 4 : 2);
    return p.limits.maxPerStageDescriptorStorageImages >= count + 37 &&
           p.limits.maxDescriptorSetStorageImages >= count + 37 &&
           p.limits.maxPerStageResources >= 4096 + count + 42;
  }
  uint32_t Renderer::Gpu::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw VK_ERROR_FEATURE_NOT_PRESENT;
  }
  std::unique_ptr<Texture> Renderer::Gpu::texture(VkFormat format, VkExtent2D size) {
    auto t = std::make_unique<Texture>();
    t->device = device;
    t->extent = size;
    VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                         .imageType = VK_IMAGE_TYPE_2D,
                         .format = format,
                         .extent = {size.width, size.height, 1},
                         .mipLevels = 1,
                         .arrayLayers = 1,
                         .samples = VK_SAMPLE_COUNT_1_BIT,
                         .tiling = VK_IMAGE_TILING_OPTIMAL,
                         .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    check(vkCreateImage(device, &ci, nullptr, &t->image));
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(device, t->image, &r);
    VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                            .allocationSize = r.size,
                            .memoryTypeIndex =
                                memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(device, &ai, nullptr, &t->memory));
    check(vkBindImageMemory(device, t->image, t->memory, 0));
    t->bytes = r.size;
    VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                             .image = t->image,
                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
                             .format = format,
                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(device, &vi, nullptr, &t->view));
    stats.memoryBytes += r.size;
    return t;
  }
  std::unique_ptr<Buffer> Renderer::Gpu::buffer(VkDeviceSize size) {
    auto b = std::make_unique<Buffer>();
    b->device = device;
    b->size = size;
    VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                          .size = size,
                          .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    check(vkCreateBuffer(device, &ci, nullptr, &b->buffer));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(device, b->buffer, &r);
    VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                            .allocationSize = r.size,
                            .memoryTypeIndex =
                                memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    check(vkAllocateMemory(device, &ai, nullptr, &b->memory));
    check(vkBindBufferMemory(device, b->buffer, b->memory, 0));
    check(vkMapMemory(device, b->memory, 0, size, 0, &b->mapped));
    stats.memoryBytes += r.size;
    return b;
  }
  VkPipeline Renderer::Gpu::pipeline(VkPipelineLayout pl, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule module{};
    check(pipeline::createShaderModule(device, &ci, nullptr, &module));
    VkComputePipelineCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                   .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                             .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                             .module = module,
                                             .pName = "main"},
                                   .layout = pl};
    VkPipeline result{};
    auto status = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1,
                                                   &pi, nullptr, &result);
    pipeline::destroyShaderModule(device, module, nullptr);
    check(status);
    return result;
  }
#ifdef REMIX_HAS_NRD
  NrdPass::~NrdPass() {
    for (auto &p : pipelines) {
      if (p.pipeline)
        pipeline::destroyPipeline(device, p.pipeline, nullptr);
      if (p.layout)
        pipeline::destroyPipelineLayout(device, p.layout, nullptr);
      if (p.resources)
        pipeline::destroyDescriptorSetLayout(device, p.resources, nullptr);
    }
    for (auto &frame : frames)
      if (frame.pool)
        vkDestroyDescriptorPool(device, frame.pool, nullptr);
    if (nrdPool)
      vkDestroyDescriptorPool(device, nrdPool, nullptr);
    if (commonLayout)
      pipeline::destroyDescriptorSetLayout(device, commonLayout, nullptr);
    for (auto sampler : samplers)
      if (sampler)
        pipeline::destroySampler(device, sampler, nullptr);
    if (instance)
      nrd::DestroyInstance(*instance);
  }
#endif
  Renderer::Gpu::~Gpu() {
    for (auto &frame : frames)
      if (frame.timestamps)
        vkDestroyQueryPool(device, frame.timestamps, nullptr);
    if (timestamps)
      vkDestroyQueryPool(device, timestamps, nullptr);
    if (post)
      pipeline::destroyPipeline(device, post, nullptr);
    if (layout)
      pipeline::destroyPipelineLayout(device, layout, nullptr);
    if (pool)
      vkDestroyDescriptorPool(device, pool, nullptr);
    if (inputLayout)
      pipeline::destroyDescriptorSetLayout(device, inputLayout, nullptr);
    if (emptyLayout)
      pipeline::destroyDescriptorSetLayout(device, emptyLayout, nullptr);
  }
  Renderer::Renderer() = default;
  Renderer::~Renderer() = default;
  void Renderer::initialize(VkPhysicalDevice physical, VkDevice device,
                            VkExtent2D extent, bool reblur,
                            uint32_t queueFamily, bool sigma, bool local,
                            bool localDenoise, bool split,
                            bool rayReconstruction, bool validation,
                            bool localReconstruction, bool foregroundDenoise) {
    m_gpu = std::make_unique<Gpu>();
    auto &g = *m_gpu;
    g.physical = physical;
    g.device = device;
    g.extent = extent;
    g.sigma = sigma;
    g.local = local;
    g.split = split && local;
    g.validation = validation;
    g.localReconstruction = localReconstruction;
    foregroundDenoise |= reblur;
    vkGetPhysicalDeviceMemoryProperties(physical, &g.memory);
    vkGetPhysicalDeviceProperties(physical, &g.properties);
    if (extent.width > 65535 || extent.height > 65535)
      throw VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (uint32_t i = 0; i < ImageCount; ++i) {
      if ((i >= Penumbra && i < SigmaImageCount && !sigma) ||
          (i >= LocalInput && i < LocalImageCount && !local) ||
          (i >= EmissiveInput && i < EmissiveImageCount && !g.split))
        continue;
      VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
      if (i == Normal || i == PreviousNormal || i == FrontNormal)
        format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      if (i == Depth || i == PreviousDepth || i == Confidence || i == RrHitDistance || i == FrontDepth || i == FrontConfidence ||
          i == LocalConfidence)
        format = VK_FORMAT_R32_SFLOAT;
      if (i == Motion || i == FrontMotion || i == LocalMotion)
        format = VK_FORMAT_R32G32B32A32_SFLOAT;
      if (i == PsrMirror)
        format = VK_FORMAT_R32G32B32A32_UINT;
      if (i == Penumbra)
        format = VK_FORMAT_R16_SFLOAT;
      if (i == ShadowInput || i == ShadowOutput)
        format = VK_FORMAT_R8G8B8A8_UNORM;
      VkExtent2D size = ((i >= RrDiffuse && i < FrontNormal && !rayReconstruction) ||
                         (i >= FrontNormal && !foregroundDenoise) || (i >= PbrSpecularSh1 && !reblur))
                            ? VkExtent2D{1, 1} : extent;
      if (i == GradientA || i == GradientB || ((i == MovingA || i == MovingB) && reblur))
        size = {(extent.width + 4) / 5, (extent.height + 4) / 5};
      else if (i == MovingA || i == MovingB)
        size = {1, 1};
      if (i == LocalMotion)
        size = local ? extent : VkExtent2D{1, 1};
      g.images[i] = g.texture(format, size);
    }
    g.uniform = g.buffer(sizeof(Frame));
    for (unsigned i = 1; i < gpuFrameCount; ++i)
      g.frames[i].uniform = g.buffer(sizeof(Frame));
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < BaseImageCount + 3; ++i)
      bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    bindings.push_back(
        {BaseImageCount + 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    if (sigma)
      for (uint32_t i = 26; i < 29; ++i)
        bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    if (local)
      for (uint32_t i = 29; i < 31; ++i)
        bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    if (g.split)
      for (uint32_t i = 31; i < 33; ++i)
        bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    for (uint32_t i = 33; i < ImageCount + 4; ++i)
      bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    check(pipeline::createDescriptorSetLayout(device, &ci, nullptr,
                                              &g.emptyLayout));
    ci.bindingCount = uint32_t(bindings.size());
    ci.pBindings = bindings.data();
    check(pipeline::createDescriptorSetLayout(device, &ci, nullptr,
                                              &g.inputLayout));
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         BaseImageCount + 29 + (sigma ? 3u : 0u) + (local ? 2u : 0u) + (g.split ? 2u : 0u)},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    for (auto &size : sizes)
      size.descriptorCount *= gpuFrameCount;
    VkDescriptorPoolCreateInfo pc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                  .maxSets = gpuFrameCount,
                                  .poolSizeCount = 2,
                                  .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &pc, nullptr, &g.pool));
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                   .descriptorPool = g.pool,
                                   .descriptorSetCount = 1,
                                   .pSetLayouts = &g.inputLayout};
    check(vkAllocateDescriptorSets(device, &ai, &g.inputs));
    for (unsigned i = 1; i < gpuFrameCount; ++i)
      check(vkAllocateDescriptorSets(device, &ai, &g.frames[i].inputs));
    VkDescriptorSetLayout layouts[] = {g.emptyLayout, g.inputLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                  .setLayoutCount = 2,
                                  .pSetLayouts = layouts,
                                  .pushConstantRangeCount = 1,
                                  .pPushConstantRanges = &push};
    check(pipeline::createPipelineLayout(device, &li, nullptr, &g.layout));
    g.post = g.split ? (sigma ? g.pipeline(g.layout, splitLocalSigmaShader, sizeof(splitLocalSigmaShader))
                              : g.pipeline(g.layout, splitLocalShader, sizeof(splitLocalShader)))
             : local ? (sigma ? g.pipeline(g.layout, localSigmaShader, sizeof(localSigmaShader))
                              : g.pipeline(g.layout, localShader, sizeof(localShader)))
             : sigma ? g.pipeline(g.layout, sigmaShader, sizeof(sigmaShader))
                     : g.pipeline(g.layout, compShader, sizeof(compShader));
#ifdef REMIX_HAS_NRD
    if (reblur) {
      g.indirect = std::make_unique<NrdPass>();
      g.initializeNrd(*g.indirect, false);
      g.pbr = std::make_unique<NrdPass>();
      g.initializeNrd(*g.pbr, false, false, true);
    }
    if (foregroundDenoise) {
      g.foreground = std::make_unique<NrdPass>();
      g.foreground->foreground = true;
      g.initializeNrd(*g.foreground, false, false, true);
    }
    if (localDenoise) {
      g.localDirect = std::make_unique<NrdPass>();
      g.initializeNrd(*g.localDirect, false, true);
      if (g.split) {
        g.emissiveDirect = std::make_unique<NrdPass>();
        g.emissiveDirect->emissive = true;
        g.initializeNrd(*g.emissiveDirect, false, true);
      }
    }
    if (sigma) {
      g.shadow = std::make_unique<NrdPass>();
      g.initializeNrd(*g.shadow, true);
    }
#else
    (void)reblur;
    (void)localDenoise;
    (void)foregroundDenoise;
#endif
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    g.timestampBits = queues.at(queueFamily).timestampValidBits;
    if (g.timestampBits && g.properties.limits.timestampComputeAndGraphics) {
      VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                               .queryType = VK_QUERY_TYPE_TIMESTAMP,
                               .queryCount = 11};
      check(vkCreateQueryPool(device, &qi, nullptr, &g.timestamps));
      for (unsigned i = 1; i < gpuFrameCount; ++i)
        check(vkCreateQueryPool(device, &qi, nullptr, &g.frames[i].timestamps));
    }
    g.stats.peakMemoryBytes = g.stats.memoryBytes;
  }
  VkDescriptorSetLayout Renderer::inputLayout() const { return m_gpu->inputLayout; }
  upscale::Input Renderer::image(ImageSlot slot) const {
    const auto &image = *m_gpu->images.at(slot);
    return {image.image, image.view};
  }
  void Renderer::Gpu::swapFrame(FrameResources &next, [[maybe_unused]] uint32_t slot) {
    using std::swap;
    swap(uniform, next.uniform);
    swap(inputs, next.inputs);
    swap(timestamps, next.timestamps);
    swap(timingPending, next.timingPending);
#ifdef REMIX_HAS_NRD
    for (auto *pass : {indirect.get(), shadow.get(), localDirect.get(), emissiveDirect.get(), pbr.get(), foreground.get()})
      if (pass) {
        swap(pass->nrdPool, pass->frames[slot].pool);
        swap(pass->constants, pass->frames[slot].constants);
      }
#endif
  }
  void Renderer::selectFrame(uint32_t slot) {
    auto &g = *m_gpu;
    if (slot == g.frameIndex)
      return;
    g.swapFrame(g.frames[g.frameIndex], g.frameIndex);
    g.frameIndex = slot;
    g.swapFrame(g.frames[slot], slot);
  }
  VkDescriptorSet Renderer::inputs() const { return m_gpu->inputs; }
  void Renderer::begin(VkCommandBuffer command, const Frame &frame, const scene::Matrix &projection,
                       const scene::Matrix &previousProjection, VkImageView world, VkImageView sky,
                       bool localFireflyFilter) {
    auto &g = *m_gpu;
    g.frame = frame;
    g.localFireflyFilter = localFireflyFilter;
    g.frame.flags[0] |= !g.initialized;
    // NRD's history lengths are seconds converted with a smoothed frame rate
    // (a single hitch counts as at most twice the current frame time).
    g.smoothedFrameMilliseconds =
        g.smoothedFrameMilliseconds > 0
            ? g.smoothedFrameMilliseconds +
                  (std::min(frame.timing[0], 2 * g.smoothedFrameMilliseconds) - g.smoothedFrameMilliseconds) * .05f
            : frame.timing[0];
    // PBR paths select one lobe only where REBLUR_DIFFUSE_SPECULAR_SH consumes
    // them with its prepass on (see prepareNrd): shaders and settings agree.
    g.frame.translucency[3] =
        g.frame.flags[1] && g.frame.hitDistance[3] == 0 && !(localFireflyFilter && g.frame.local[1]);
    if (g.timingPending) {
      uint64_t values[11];
      auto r = vkGetQueryPoolResults(g.device, g.timestamps, 0, 11, sizeof(values), values, 8,
                                     VK_QUERY_RESULT_64_BIT);
      if (r == VK_ERROR_DEVICE_LOST)
        throw r;
      if (r == VK_SUCCESS) {
        const uint64_t mask = g.timestampBits == 64 ? UINT64_MAX : (uint64_t(1) << g.timestampBits) - 1;
        g.stats.gpuConfidenceMilliseconds =
            float((values[1] - values[0]) & mask) * g.properties.limits.timestampPeriod / 1e6f;
        g.stats.gpuDenoiseMilliseconds =
            float(((values[2] - values[1]) & mask) + ((values[10] - values[9]) & mask)) * g.properties.limits.timestampPeriod / 1e6f;
        g.stats.gpuTaaMilliseconds =
            float((values[4] - values[3]) & mask) * g.properties.limits.timestampPeriod / 1e6f;
        g.sigmaMilliseconds =
            g.sigma ? float((values[6] - values[5]) & mask) * g.properties.limits.timestampPeriod / 1e6f : 0;
        g.localMilliseconds =
            g.local ? float((values[8] - values[7]) & mask) * g.properties.limits.timestampPeriod / 1e6f : 0;
        g.stats.gpuTimingsAvailable = 1;
      }
    }
    if (g.timestamps)
      vkCmdResetQueryPool(command, g.timestamps, 0, 11);
    if (!g.initialized) {
      std::vector<Texture *> textures;
      for (auto &t : g.images)
        if (t)
          textures.push_back(t.get());
#ifdef REMIX_HAS_NRD
      for (auto *pass :
           {g.indirect.get(), g.shadow.get(), g.localDirect.get(), g.emissiveDirect.get(), g.pbr.get(), g.foreground.get()}) {
        if (!pass)
          continue;
        for (auto &t : pass->permanent)
          textures.push_back(t.get());
        for (auto &t : pass->transient)
          textures.push_back(t.get());
      }
#endif
      for (auto *t : textures) {
        VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = t->image,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
        VkClearColorValue zero{};
        vkCmdClearColorImage(command, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &b.subresourceRange);
      }
      g.initialized = true;
    }
    synchronize(command);
    std::memcpy(g.uniform->mapped, &g.frame, sizeof(Frame));
    std::vector<VkDescriptorImageInfo> images;
    for (uint32_t i = 0; i < BaseImageCount; ++i)
      images.push_back({VK_NULL_HANDLE, g.images[i]->view, VK_IMAGE_LAYOUT_GENERAL});
    images.push_back({VK_NULL_HANDLE, world, VK_IMAGE_LAYOUT_GENERAL});
    images.push_back({VK_NULL_HANDLE, sky, VK_IMAGE_LAYOUT_GENERAL});
    images.push_back(images[0]);
    std::vector<uint32_t> imageBindings;
    for (uint32_t i = 0; i < images.size(); ++i)
      imageBindings.push_back(i);
    for (uint32_t i = Penumbra; i < ImageCount; ++i)
      if (g.images[i]) {
        images.push_back({VK_NULL_HANDLE, g.images[i]->view, VK_IMAGE_LAYOUT_GENERAL});
        imageBindings.push_back(i + 4);
      }
    std::vector<VkWriteDescriptorSet> writes;
    for (uint32_t i = 0; i < images.size(); ++i)
      writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = g.inputs,
                        .dstBinding = imageBindings[i],
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .pImageInfo = &images[i]});
    VkDescriptorBufferInfo bi{g.uniform->buffer, 0, sizeof(Frame)};
    writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                      .dstSet = g.inputs,
                      .dstBinding = BaseImageCount + 3,
                      .descriptorCount = 1,
                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                      .pBufferInfo = &bi});
    vkUpdateDescriptorSets(g.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
#ifdef REMIX_HAS_NRD
    if (g.indirect) {
      if (g.frame.flags[1] && g.frame.shadow[3])
        g.prepareNrd(*g.indirect, projection, previousProjection);
      else
        g.indirect->wasActive = false;
    }
    if (g.pbr) {
      if (g.frame.flags[1] && g.frame.shadow[2])
        g.prepareNrd(*g.pbr, projection, previousProjection);
      else
        g.pbr->wasActive = false;
    }
    if (g.localDirect) {
      if (g.frame.shadow[3] && g.frame.local[0] && (g.split ? g.frame.families[0] : g.frame.local[1]))
        g.prepareNrd(*g.localDirect, projection, previousProjection);
      else
        g.localDirect->wasActive = false;
    }
    if (g.emissiveDirect) {
      if (g.frame.shadow[3] && g.frame.local[0] && g.frame.families[1])
        g.prepareNrd(*g.emissiveDirect, projection, previousProjection);
      else
        g.emissiveDirect->wasActive = false;
    }
    if (g.foreground) {
      if (g.frame.translucency[0])
        g.prepareNrd(*g.foreground, projection, previousProjection);
      else
        g.foreground->wasActive = false;
    }
    if (g.sigma)
      g.prepareNrd(*g.shadow, projection, previousProjection);
#else
    (void)projection;
    (void)previousProjection;
#endif
  }
  void Renderer::Gpu::pass(VkCommandBuffer command, uint32_t mode, uint32_t step) {
    synchronize(command);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, post);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 1, 1, &inputs, 0, nullptr);
    const uint32_t push[] = {mode, step};
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, push);
    auto size = mode == 0 ? images[GradientA]->extent : extent;
    vkCmdDispatch(command, (size.width + 7) / 8, (size.height + 7) / 8, 1);
  }
  void Renderer::setLocalTuning(const LocalTuning &tuning) { m_gpu->localTuning = tuning; }
  void Renderer::beginConfidence(VkCommandBuffer command) {
    if (m_gpu->timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_gpu->timestamps, 0);
  }
  void Renderer::recordOpaque(VkCommandBuffer command) {
    auto &g = *m_gpu;
    if (g.frame.flags[1]) {
      gpu::GpuScope scope(command, "Indirect confidence filter", g.extent);
      for (uint32_t i = 1; i <= 5; ++i)
        g.pass(command, 0, i);
      g.pass(command, 1);
    }
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 1);
#ifdef REMIX_HAS_NRD
    if (g.frame.flags[1] && g.frame.shadow[3]) {
      gpu::GpuScope scope(command, "NRD REBLUR_DIFFUSE_SH (indirect)", g.extent);
      g.recordNrd(*g.indirect, command);
    }
    if (g.frame.flags[1] && g.frame.shadow[2]) {
      gpu::GpuScope scope(command, "NRD REBLUR_DIFFUSE_SPECULAR_SH (PBR)", g.extent);
      g.recordNrd(*g.pbr, command);
    }
#endif
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 2);
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 5);
#ifdef REMIX_HAS_NRD
    if (g.sigma) {
      gpu::GpuScope scope(command, "NRD SIGMA (sun shadow)", g.extent);
      g.recordNrd(*g.shadow, command);
    }
#endif
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 6);
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 7);
#ifdef REMIX_HAS_NRD
    if (g.localDirect && g.frame.shadow[3] && g.frame.local[0] &&
        (g.split ? g.frame.families[0] : g.frame.local[1])) {
      gpu::GpuScope scope(command, g.split ? "NRD REBLUR_DIFFUSE (torch lights)" : "NRD REBLUR_DIFFUSE (local lights)",
                          g.extent);
      g.recordNrd(*g.localDirect, command);
    }
    if (g.emissiveDirect && g.frame.shadow[3] && g.frame.local[0] && g.frame.families[1]) {
      gpu::GpuScope scope(command, "NRD REBLUR_DIFFUSE (emissive lights)", g.extent);
      g.recordNrd(*g.emissiveDirect, command);
    }
#endif
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 8);
    // The transparency tracer can reuse this frame's already filtered opaque
    // radiance. Foreground composition happens only after its own tracing/NRD.
    if (g.frame.translucency[1]) {
      gpu::GpuScope scope(command, "Translucency reuse compose", g.extent);
      g.pass(command, 2, 0);
    }
    synchronize(command);
  }
  void Renderer::record(VkCommandBuffer command) {
    auto &g = *m_gpu;
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 9);
#ifdef REMIX_HAS_NRD
    if (g.foreground && g.frame.translucency[0]) {
      gpu::GpuScope scope(command, "NRD REBLUR_DIFFUSE_SPECULAR (translucent front)", g.extent);
      g.recordNrd(*g.foreground, command);
    }
#endif
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 10);
    if (g.frame.hitDistance[3] == 0) {
      gpu::GpuScope scope(command, "Denoised lighting compose", g.extent);
      g.pass(command, 2, 1);
    } else if (g.frame.translucency[0]) {
      gpu::GpuScope scope(command, "DLSS-RR foreground compose", g.extent);
      g.pass(command, 5);
    }
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 3);
    if (g.frame.hitDistance[3] == 0) {
      gpu::GpuScope scope(command, g.frame.flags[2] ? "TAA" : "TAA pass (history only, TAA off)", g.extent);
      g.pass(command, 3);
    }
    {
      gpu::GpuScope scope(command, "Denoiser history copy", g.extent);
      g.pass(command, 4);
    }
    if (g.timestamps) {
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 4);
      g.timingPending = true;
    }
    synchronize(command);
    g.submittedFrame = true;
  }
  void Renderer::submitted() {
    auto &g = *m_gpu;
    if (!g.submittedFrame)
      return;
    g.submittedFrame = false;
    if (g.frame.flags[0]) {
      ++g.stats.historyResets;
      g.stats.historyFrames = 0;
    }
    if (g.sigma) {
      ++g.shadowStats.renderedFrames;
      if (g.frame.flags[0] || g.frame.shadow[0])
        ++g.shadowStats.historyResets;
    }
    ++g.stats.renderedFrames;
    ++g.stats.historyFrames;
    g.stats.activeDenoiser = g.frame.flags[1];
    g.stats.activeTaa = g.frame.flags[2];
    g.stats.validationEnabled = g.frame.timing[1] != 0;
    if (g.frame.flags[0] || g.frame.timing[2] != 0)
      g.quietFrames = 0;
    ++g.quietFrames;
    g.stats.redrawRequested = g.quietFrames < g.warmupFrames;
  }
  remixapi_ShadowStatisticsEXT Renderer::shadowStatistics() const {
    auto result = m_gpu->shadowStats;
    result.active = m_gpu->sigma;
    result.debugView = m_gpu->frame.shadow[1];
    result.gpuTimingsAvailable = m_gpu->stats.gpuTimingsAvailable;
    result.gpuSigmaMilliseconds = m_gpu->sigmaMilliseconds;
    return result;
  }
  bool Renderer::foregroundActive() const { return m_gpu->frame.translucency[0] != 0; }
  float Renderer::localMilliseconds() const { return m_gpu->localMilliseconds; }
  remixapi_DenoisingStatisticsEXT Renderer::statistics() const { return m_gpu->stats; }
} // namespace remix::denoise
