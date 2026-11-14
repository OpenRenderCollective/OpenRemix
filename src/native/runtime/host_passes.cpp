#include "runtime/host_passes.h"
#if !defined(REMIX_METAL)
#include "runtime/pipelines.h"
#endif
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace remix {
  namespace {
    constexpr uint32_t kMaxBindings = 4, kMaxAttributes = 16, kMaxBuffers = 8, kMaxSamplers = 21;
#if !defined(REMIX_METAL)
    // A host shader binding's descriptor type (REMIXAPI_HOST_BINDING_*).
    VkDescriptorType descriptorType(uint32_t type) {
      switch (type) {
      case REMIXAPI_HOST_BINDING_UNIFORM_BUFFER_EXT:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      case REMIXAPI_HOST_BINDING_SAMPLED_IMAGE_EXT:
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      case REMIXAPI_HOST_BINDING_SAMPLER_STATE_EXT:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
      default:
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      }
    }
    void check(VkResult result) {
      if (result != VK_SUCCESS)
        throw result;
    }
    constexpr VkDeviceSize kSlotMinimum = 256 * 1024;
    // The frame's copy keeps the swapchain's channel order, raw (UNORM): the
    // host's shaders read and write its bytes as D3D9 does without sRGB states.
    VkFormat unorm(VkFormat format) {
      switch (format) {
      case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
      case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
      default:
        return format;
      }
    }
    bool fits(const int32_t rect[4], VkExtent2D extent) {
      return rect[2] == 0 ||
             (rect[0] >= 0 && rect[1] >= 0 && rect[2] > 0 && rect[3] > 0 &&
              uint32_t(rect[0] + rect[2]) <= extent.width && uint32_t(rect[1] + rect[3]) <= extent.height);
    }
#endif
  } // namespace

#if !defined(REMIX_METAL)

  void HostPasses::initialize(VkPhysicalDevice physical, VkDevice device) {
    m_physical = physical;
    m_device = device;
    vkGetPhysicalDeviceMemoryProperties(physical, &m_memory);
  }

  void HostPasses::destroy() {
    for (auto &[handle, target] : m_targets)
      destroyTarget(target);
    m_targets.clear();
    for (auto &[frame, target] : m_retired)
      destroyTarget(target);
    m_retired.clear();
    for (auto &slot : m_slots) {
      if (slot.pool)
        vkDestroyDescriptorPool(m_device, slot.pool, nullptr);
      if (slot.mapped)
        vkUnmapMemory(m_device, slot.memory);
      if (slot.buffer)
        vkDestroyBuffer(m_device, slot.buffer, nullptr);
      if (slot.memory)
        vkFreeMemory(m_device, slot.memory, nullptr);
      slot = {};
    }
    // Pipelines, layouts, samplers and modules belong to the pipeline service.
    m_pipelines.clear();
    m_layouts.clear();
    std::lock_guard lock(m_mutex);
    m_shaders.clear();
    m_pending.clear();
    m_device = VK_NULL_HANDLE;
  }

  uint32_t HostPasses::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t type = 0; type < m_memory.memoryTypeCount; ++type)
      if ((bits & (1u << type)) && (m_memory.memoryTypes[type].propertyFlags & flags) == flags)
        return type;
    throw VK_ERROR_FEATURE_NOT_PRESENT;
  }

  bool HostPasses::ready() const { return m_device != VK_NULL_HANDLE; }

  bool HostPasses::createModule(Shader &shader) {
    static_assert(formatRgba8 == VK_FORMAT_R8G8B8A8_UNORM && formatBgra8 == VK_FORMAT_B8G8R8A8_UNORM);
    shader.stage = shader.vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkShaderModuleCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                      .codeSize = shader.code.size() * 4,
                                      .pCode = shader.code.data()};
    return pipeline::createShaderModule(m_device, &ci, nullptr, &shader.module) == VK_SUCCESS;
  }
#endif

  remixapi_ErrorCode HostPasses::registerShader(const remixapi_HostShaderInfoEXT &info,
                                                remixapi_HostShaderHandleEXT &out) {
    out = 0;
    if (!ready() || !info.code || info.codeBytes < 20 || info.codeBytes % 4 || info.codeBytes > (64u << 20) ||
        (info.stage != REMIXAPI_HOST_SHADER_VERTEX_EXT && info.stage != REMIXAPI_HOST_SHADER_FRAGMENT_EXT) ||
        (info.bindingCount && !info.bindings) || info.bindingCount > 128 || info.pushConstantBytes > 128)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    // Binding numbers as the host's compiler gives them: DXVK's D3D11 layout
    // numbers a stage's slots past the others' (a vertex shader's constant
    // buffers from 160).
    for (uint32_t i = 0; i < info.bindingCount; ++i)
      if (info.bindings[i].type > REMIXAPI_HOST_BINDING_SAMPLER_STATE_EXT || info.bindings[i].binding > 4095)
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    Shader shader;
    shader.vertex = info.stage == REMIXAPI_HOST_SHADER_VERTEX_EXT;
    shader.code.assign(info.code, info.code + info.codeBytes / 4);
    shader.bindings.assign(info.bindings, info.bindings + info.bindingCount);
    shader.pushBytes = info.pushConstantBytes;
    if (!createModule(shader))
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    std::lock_guard lock(m_mutex);
    out = m_nextShader++;
    m_shaders.emplace(out, std::make_shared<Shader>(std::move(shader)));
    m_stats.shaders = uint32_t(m_shaders.size());
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode HostPasses::destroyShader(remixapi_HostShaderHandleEXT shader) {
    std::lock_guard lock(m_mutex);
    if (!m_shaders.erase(shader))
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    m_stats.shaders = uint32_t(m_shaders.size());
    return REMIXAPI_ERROR_CODE_SUCCESS; // its modules and pipelines stay interned
  }

  void HostPasses::setEnabled(bool enabled) {
    if (m_enabled.exchange(enabled) == enabled || enabled)
      return;
    // Off: only the passes replayed regardless stay (with the uploads, which
    // they may sample).
    std::lock_guard lock(m_mutex);
    std::erase_if(m_pending, [](const Pass &pass) { return !pass.always; });
  }

  remixapi_ErrorCode HostPasses::submit(const remixapi_HostPassListEXT &list) {
    if ((list.passCount && !list.passes) || list.passCount > 1024)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    // The passes' flags (remixapi_HostPassListFlagsEXT), when the list has them.
    const uint32_t *flags = nullptr;
    uint32_t flagCount = 0;
    for (auto *next = static_cast<const remixapi_HostPassListFlagsEXT *>(list.pNext); next;
         next = static_cast<const remixapi_HostPassListFlagsEXT *>(next->pNext))
      if (next->sType == REMIXAPI_STRUCT_TYPE_HOST_PASS_LIST_FLAGS_EXT) {
        if (next->flagCount && !next->flags)
          return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        flags = next->flags;
        flagCount = std::min(next->flagCount, list.passCount);
      }
    // The setting off: only the passes flagged to run regardless; the rest are
    // dropped unchecked, as before the flag.
    const bool enabled = m_enabled;
    uint32_t always = 0, dropped = 0;
    for (uint32_t i = 0; i < flagCount; ++i)
      always += (flags[i] & REMIXAPI_HOST_PASS_ALWAYS_EXT) != 0;
    if (!enabled && !always) {
      std::lock_guard lock(m_mutex);
      m_pending.clear();
      m_stats.passes = m_stats.draws = m_stats.copies = m_stats.clears = m_stats.skipped = 0;
      m_stats.skippedCopySource = m_stats.skippedShaders = m_stats.skippedPipeline = m_stats.skippedDescriptors = 0;
      m_stats.skippedBuffer = m_stats.skippedSampler = m_stats.skippedTarget = m_stats.skippedViewDepth = 0;
      m_stats.always = 0;
      m_stats.dropped = list.passCount;
      return REMIXAPI_ERROR_CODE_SUCCESS; // the setting is off: dropped
    }
    std::vector<Pass> passes;
    passes.reserve(enabled ? list.passCount : always);
    std::lock_guard lock(m_mutex);
    for (uint32_t i = 0; i < list.passCount; ++i) {
      const auto &info = list.passes[i];
      Pass pass;
      pass.info = info;
      pass.always = i < flagCount && (flags[i] & REMIXAPI_HOST_PASS_ALWAYS_EXT);
      if (!enabled && !pass.always) {
        ++dropped;
        continue;
      }
      const bool frameDepthTarget = info.target == REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT;
      if (info.kind > REMIXAPI_HOST_PASS_CLEAR_EXT ||
          (info.target >= REMIXAPI_HOST_TARGET_RESERVED_EXT && !frameDepthTarget) ||
          (info.kind == REMIXAPI_HOST_PASS_COPY_EXT &&
           (info.source >= REMIXAPI_HOST_TARGET_RESERVED_EXT || frameDepthTarget)) ||
          (info.depth && ((info.depth & 0xff) > 8 || (info.target && !frameDepthTarget))))
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      if (info.target && !frameDepthTarget &&
          (!info.targetWidth || !info.targetHeight || info.targetWidth > 16384 || info.targetHeight > 16384 ||
           info.targetChannels > REMIXAPI_HOST_CHANNELS_NO_ALPHA_EXT))
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      if (info.kind == REMIXAPI_HOST_PASS_DRAW_EXT) {
        if (!m_shaders.count(info.vertexShader) || !m_shaders.count(info.fragmentShader) ||
            info.bufferCount > kMaxBuffers || info.samplerCount > kMaxSamplers ||
            info.vertexBindingCount > kMaxBindings || info.attributeCount > kMaxAttributes ||
            (info.bufferCount && !info.buffers) || (info.samplerCount && !info.samplers) ||
            (info.vertexBindingCount && !info.vertexBindings) || (info.attributeCount && !info.attributes) ||
            !info.indices || !info.indexCount || info.indexCount > (1u << 20) || info.topology > 1 ||
            info.pushConstantBytes > 128 || (info.pushConstantBytes && !info.pushConstants) ||
            info.srcColorBlend > 18 || info.dstColorBlend > 18 || info.srcAlphaBlend > 18 || info.dstAlphaBlend > 18 ||
            info.colorBlendOp > 4 || info.alphaBlendOp > 4)
          return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        pass.buffers.assign(info.buffers, info.buffers + info.bufferCount);
        pass.samplers.assign(info.samplers, info.samplers + info.samplerCount);
        pass.vertexBindings.assign(info.vertexBindings, info.vertexBindings + info.vertexBindingCount);
        pass.attributes.assign(info.attributes, info.attributes + info.attributeCount);
        size_t bytes = info.pushConstantBytes;
        for (const auto &b : pass.buffers) {
          if (!b.data || !b.bytes || b.bytes > (1u << 20))
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
          bytes += b.bytes;
        }
        for (const auto &v : pass.vertexBindings) {
          if (!v.data || !v.bytes || !v.stride || v.bytes > (16u << 20))
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
          bytes += v.bytes;
        }
        for (const auto &a : pass.attributes)
          if (a.binding >= info.vertexBindingCount || a.location > 31)
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        for (const auto &s : pass.samplers)
          if (s.addressU > 2 || s.addressV > 2 ||
              (s.target >= REMIXAPI_HOST_TARGET_RESERVED_EXT && s.target != REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT))
            return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        // One copy of every byte, in a known order (see record).
        pass.data.reserve(bytes);
        auto append = [&](const void *data, size_t size) {
          const auto *p = static_cast<const uint8_t *>(data);
          pass.data.insert(pass.data.end(), p, p + size);
        };
        for (const auto &b : pass.buffers)
          append(b.data, b.bytes);
        if (info.pushConstantBytes)
          append(info.pushConstants, info.pushConstantBytes);
        for (const auto &v : pass.vertexBindings)
          append(v.data, v.bytes);
        pass.indices.assign(info.indices, info.indices + info.indexCount);
      }
      passes.push_back(std::move(pass));
    }
    m_pending = std::move(passes);
    // The frame's counts: what record() runs of this submission (nothing, when
    // it is empty or the frame skips the passes).
    m_stats.passes = m_stats.draws = m_stats.copies = m_stats.clears = m_stats.skipped = 0;
    m_stats.skippedCopySource = m_stats.skippedShaders = m_stats.skippedPipeline = m_stats.skippedDescriptors = 0;
    m_stats.skippedBuffer = m_stats.skippedSampler = m_stats.skippedTarget = m_stats.skippedViewDepth = 0;
    m_stats.always = always;
    m_stats.dropped = dropped;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode HostPasses::updateTarget(remixapi_HostTargetHandleEXT target, uint32_t width, uint32_t height,
                                              uint32_t format, const void *pixels, uint64_t bytes, uint32_t depth) {
    if (!target || target >= REMIXAPI_HOST_TARGET_RESERVED_EXT || !width || !height || !depth || width > 16384 ||
        height > 16384 || depth > 2048 || bytes != uint64_t(width) * height * depth * 4 ||
        (format != formatRgba8 && format != formatBgra8))
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    // Kept while the setting is off too: passes flagged
    // REMIXAPI_HOST_PASS_ALWAYS_EXT sample them (a host keeps its other work
    // for when the setting is on).
    Upload upload{target, format, width, height, depth, {}};
    upload.pixels.assign(static_cast<const uint8_t *>(pixels), static_cast<const uint8_t *>(pixels) + bytes);
    std::lock_guard lock(m_mutex);
    for (auto &existing : m_uploads) // the latest contents count
      if (existing.target == target) {
        existing = std::move(upload);
        return REMIXAPI_ERROR_CODE_SUCCESS;
      }
    m_uploads.push_back(std::move(upload));
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  bool HostPasses::pending() const {
    std::lock_guard lock(m_mutex);
    return !m_pending.empty() || !m_uploads.empty();
  }

  remixapi_HostPassStatisticsEXT HostPasses::statistics() const {
    std::lock_guard lock(m_mutex);
    return m_stats;
  }

#if !defined(REMIX_METAL)
  HostPasses::Target &HostPasses::target(remixapi_HostTargetHandleEXT handle, VkFormat format, VkExtent2D extent,
                                         uint32_t channels, uint32_t depth) {
    auto &t = m_targets[handle];
    if (t.image && t.depth > 1 && depth == 1)
      return t; // a volume stays one: a pass naming it as its target is skipped
    if (t.image && (t.format != format || t.extent.width != extent.width || t.extent.height != extent.height ||
                    t.channels != channels || t.depth != depth))
      retireTarget(t);
    if (!t.image) {
      t.format = format;
      t.extent = extent;
      t.channels = channels;
      t.depth = depth;
      const bool volume = depth > 1;
      t.initialized = false;
      const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                                    .format = format,
                                    .extent = {extent.width, extent.height, depth},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = VkImageUsageFlags(volume ? 0 : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_DST_BIT};
      check(vkCreateImage(m_device, &image, nullptr, &t.image));
      VkMemoryRequirements requirements{};
      vkGetImageMemoryRequirements(m_device, t.image, &requirements);
      const VkMemoryAllocateInfo allocation{
          .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
          .allocationSize = requirements.size,
          .memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      check(vkAllocateMemory(m_device, &allocation, nullptr, &t.memory));
      check(vkBindImageMemory(m_device, t.image, t.memory, 0));
      const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = t.image,
                                       .viewType = volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
                                       .format = format,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      check(vkCreateImageView(m_device, &view, nullptr, &t.view));
      // What passes sample: luminance formats replicate red, alpha-less ones read one.
      const VkComponentSwizzle r = VK_COMPONENT_SWIZZLE_R, one = VK_COMPONENT_SWIZZLE_ONE,
                               same = VK_COMPONENT_SWIZZLE_IDENTITY;
      VkImageViewCreateInfo sampledView = view;
      if (channels == REMIXAPI_HOST_CHANNELS_LUMINANCE_EXT)
        sampledView.components = {r, r, r, one};
      else if (channels == REMIXAPI_HOST_CHANNELS_LUMINANCE_ALPHA_EXT)
        sampledView.components = {r, r, r, same};
      else if (channels == REMIXAPI_HOST_CHANNELS_NO_ALPHA_EXT)
        sampledView.components = {same, same, same, one};
      check(vkCreateImageView(m_device, &sampledView, nullptr, &t.sampled));
      if (volume) {
        // Sampled only: no render pass or framebuffer. Its use counts from now,
        // or the end of this frame's record would take it as stale and destroy
        // it under the upload just recorded.
        t.lastFrame = m_frame;
        return t;
      }
      // Its render pass: load and keep, in GENERAL layout throughout.
      const VkAttachmentDescription attachment{.format = format,
                                               .samples = VK_SAMPLE_COUNT_1_BIT,
                                               .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                               .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
                                               .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
      const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
      const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         .colorAttachmentCount = 1,
                                         .pColorAttachments = &reference};
      const VkSubpassDependency dependencies[] = {
          {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
           VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
          {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
           VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0}};
      const VkRenderPassCreateInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                        .attachmentCount = 1,
                                        .pAttachments = &attachment,
                                        .subpassCount = 1,
                                        .pSubpasses = &subpass,
                                        .dependencyCount = 2,
                                        .pDependencies = dependencies};
      check(pipeline::createRenderPass(m_device, &pass, nullptr, &t.pass));
      const VkFramebufferCreateInfo framebuffer{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                .renderPass = t.pass,
                                                .attachmentCount = 1,
                                                .pAttachments = &t.view,
                                                .width = extent.width,
                                                .height = extent.height,
                                                .layers = 1};
      check(vkCreateFramebuffer(m_device, &framebuffer, nullptr, &t.framebuffer));
    }
    t.lastFrame = m_frame;
    return t;
  }

  HostPasses::Target &HostPasses::frameDepth(VkCommandBuffer command, VkExtent2D extent) {
    auto &t = m_targets[REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT];
    if (t.image && (t.extent.width != extent.width || t.extent.height != extent.height)) {
      retireTarget(t);
      // The frame's pass with it attached went with it.
      auto frame = m_targets.find(0);
      if (frame != m_targets.end()) {
        if (frame->second.depthFramebuffer)
          vkDestroyFramebuffer(m_device, frame->second.depthFramebuffer, nullptr);
        frame->second.depthFramebuffer = VK_NULL_HANDLE;
        frame->second.depthPass = VK_NULL_HANDLE;
      }
    }
    if (!t.image) {
      t.format = VK_FORMAT_D32_SFLOAT;
      t.extent = extent;
      t.initialized = false;
      const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = t.format,
                                    .extent = {extent.width, extent.height, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage =
                                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
      check(vkCreateImage(m_device, &image, nullptr, &t.image));
      VkMemoryRequirements requirements{};
      vkGetImageMemoryRequirements(m_device, t.image, &requirements);
      const VkMemoryAllocateInfo allocation{
          .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
          .allocationSize = requirements.size,
          .memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
      check(vkAllocateMemory(m_device, &allocation, nullptr, &t.memory));
      check(vkBindImageMemory(m_device, t.image, t.memory, 0));
      const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                       .image = t.image,
                                       .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                       .format = t.format,
                                       .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
      check(vkCreateImageView(m_device, &view, nullptr, &t.view));
      // Its own pass: depth only, loaded and kept, GENERAL throughout.
      const VkAttachmentDescription attachment{.format = t.format,
                                               .samples = VK_SAMPLE_COUNT_1_BIT,
                                               .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                               .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                               .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                               .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                               .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
                                               .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
      const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_GENERAL};
      const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         .pDepthStencilAttachment = &reference};
      const VkSubpassDependency dependencies[] = {
          {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
           VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
           VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, 0},
          {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
           VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
           VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0}};
      const VkRenderPassCreateInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                        .attachmentCount = 1,
                                        .pAttachments = &attachment,
                                        .subpassCount = 1,
                                        .pSubpasses = &subpass,
                                        .dependencyCount = 2,
                                        .pDependencies = dependencies};
      check(pipeline::createRenderPass(m_device, &pass, nullptr, &t.pass));
      const VkFramebufferCreateInfo framebuffer{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                .renderPass = t.pass,
                                                .attachmentCount = 1,
                                                .pAttachments = &t.view,
                                                .width = extent.width,
                                                .height = extent.height,
                                                .layers = 1};
      check(vkCreateFramebuffer(m_device, &framebuffer, nullptr, &t.framebuffer));
    }
    if (!t.initialized) {
      const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                         .srcAccessMask = 0,
                                         .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = t.image,
                                         .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
      t.initialized = true;
    }
    t.lastFrame = m_frame;
    return t;
  }

  void HostPasses::attachDepth(Target &frame, const Target &depth) {
    if (frame.depthFramebuffer)
      return;
    const VkAttachmentDescription attachments[] = {{.format = frame.format,
                                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                    .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                    .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
                                                   {.format = depth.format,
                                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                                    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                    .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
                                                    .finalLayout = VK_IMAGE_LAYOUT_GENERAL}};
    const VkAttachmentReference color{0, VK_IMAGE_LAYOUT_GENERAL}, depthReference{1, VK_IMAGE_LAYOUT_GENERAL};
    const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                       .colorAttachmentCount = 1,
                                       .pColorAttachments = &color,
                                       .pDepthStencilAttachment = &depthReference};
    const VkSubpassDependency dependencies[] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
         0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
         0}};
    const VkRenderPassCreateInfo pass{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                      .attachmentCount = 2,
                                      .pAttachments = attachments,
                                      .subpassCount = 1,
                                      .pSubpasses = &subpass,
                                      .dependencyCount = 2,
                                      .pDependencies = dependencies};
    check(pipeline::createRenderPass(m_device, &pass, nullptr, &frame.depthPass));
    const VkImageView views[] = {frame.view, depth.view};
    const VkFramebufferCreateInfo framebuffer{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                              .renderPass = frame.depthPass,
                                              .attachmentCount = 2,
                                              .pAttachments = views,
                                              .width = frame.extent.width,
                                              .height = frame.extent.height,
                                              .layers = 1};
    check(vkCreateFramebuffer(m_device, &framebuffer, nullptr, &frame.depthFramebuffer));
  }

  void HostPasses::retireTarget(Target &t) {
    m_retired.emplace_back(m_frame, t);
    t = {};
  }

  void HostPasses::destroyTarget(Target &t) {
    if (t.framebuffer)
      vkDestroyFramebuffer(m_device, t.framebuffer, nullptr);
    if (t.depthFramebuffer)
      vkDestroyFramebuffer(m_device, t.depthFramebuffer, nullptr);
    if (t.view)
      vkDestroyImageView(m_device, t.view, nullptr);
    if (t.sampled)
      vkDestroyImageView(m_device, t.sampled, nullptr);
    if (t.image)
      vkDestroyImage(m_device, t.image, nullptr);
    if (t.memory)
      vkFreeMemory(m_device, t.memory, nullptr);
    t = {};
  }

  const HostPasses::Layout &HostPasses::layout(const Shader &vertex, const Shader &fragment) {
    // Found by the shaders' handles... keyed by the modules, which outlive handles.
    const auto key = std::make_pair(remixapi_HostShaderHandleEXT(uint64_t(vertex.module)),
                                    remixapi_HostShaderHandleEXT(uint64_t(fragment.module)));
    auto found = m_layouts.find(key);
    if (found != m_layouts.end())
      return found->second;
    Layout layout;
    std::map<uint32_t, uint32_t> types; // binding -> type
    for (const auto *shader : {&vertex, &fragment})
      for (const auto &b : shader->bindings)
        types[b.binding] = b.type;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const auto &[binding, type] : types) {
      bindings.push_back(
          {binding, descriptorType(type), 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
      layout.bindings.push_back({binding, type});
    }
    const VkDescriptorSetLayoutCreateInfo set{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .bindingCount = uint32_t(bindings.size()),
                                              .pBindings = bindings.data()};
    check(pipeline::createDescriptorSetLayout(m_device, &set, nullptr, &layout.set));
    layout.pushBytes = std::max(vertex.pushBytes, fragment.pushBytes);
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, layout.pushBytes};
    const VkPipelineLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = 1,
                                          .pSetLayouts = &layout.set,
                                          .pushConstantRangeCount = layout.pushBytes ? 1u : 0u,
                                          .pPushConstantRanges = &range};
    check(pipeline::createPipelineLayout(m_device, &info, nullptr, &layout.pipeline));
    return m_layouts.emplace(key, std::move(layout)).first->second;
  }

  VkPipeline HostPasses::pipeline(const Pass &pass, const Shader &vertex, const Shader &fragment, const Layout &layout,
                                  VkRenderPass renderPass, bool depthOnly) {
    const auto &i = pass.info;
    // The key: everything the pipeline is made of (the service interns by
    // content as well; this spares its hashing of the shaders each frame).
    std::string key;
    auto add = [&](const void *data, size_t size) { key.append(static_cast<const char *>(data), size); };
    add(&vertex.module, sizeof(vertex.module));
    add(&fragment.module, sizeof(fragment.module));
    add(&renderPass, sizeof(renderPass));
    add(&i.topology, sizeof(uint32_t) * 9); // topology through colorWriteMask
    add(&i.depth, sizeof(i.depth));
    add(&depthOnly, sizeof(depthOnly));
    for (const auto &v : pass.vertexBindings)
      add(&v.stride, sizeof(v.stride));
    for (const auto &a : pass.attributes)
      add(&a, sizeof(a));
    auto found = m_pipelines.find(key);
    if (found != m_pipelines.end())
      return found->second;
    const VkPipelineShaderStageCreateInfo stages[] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                       .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                       .module = vertex.module,
                                                       .pName = "main"},
                                                      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                       .module = fragment.module,
                                                       .pName = "main"}};
    VkVertexInputBindingDescription bindings[kMaxBindings];
    for (uint32_t b = 0; b < pass.vertexBindings.size(); ++b)
      bindings[b] = {b, pass.vertexBindings[b].stride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[kMaxAttributes];
    for (uint32_t a = 0; a < pass.attributes.size(); ++a)
      attributes[a] = {pass.attributes[a].location, pass.attributes[a].binding, VkFormat(pass.attributes[a].format),
                       pass.attributes[a].offset};
    const VkPipelineVertexInputStateCreateInfo input{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = uint32_t(pass.vertexBindings.size()),
        .pVertexBindingDescriptions = bindings,
        .vertexAttributeDescriptionCount = uint32_t(pass.attributes.size()),
        .pVertexAttributeDescriptions = attributes};
    const VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = i.topology ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo raster{.sType =
                                                            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                        .polygonMode = VK_POLYGON_MODE_FILL,
                                                        .cullMode = VK_CULL_MODE_NONE,
                                                        .frontFace = VK_FRONT_FACE_CLOCKWISE,
                                                        .lineWidth = 1};
    const VkPipelineMultisampleStateCreateInfo samples{.sType =
                                                           VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                       .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = i.depth != 0,
        .depthWriteEnable = (i.depth & REMIXAPI_HOST_DEPTH_WRITE_EXT) != 0,
        .depthCompareOp = VkCompareOp(i.depth ? (i.depth & 0xff) - 1 : 0)};
    const VkPipelineColorBlendAttachmentState blend{.blendEnable = i.blendEnable != 0,
                                                    .srcColorBlendFactor = VkBlendFactor(i.srcColorBlend),
                                                    .dstColorBlendFactor = VkBlendFactor(i.dstColorBlend),
                                                    .colorBlendOp = VkBlendOp(i.colorBlendOp),
                                                    .srcAlphaBlendFactor = VkBlendFactor(i.srcAlphaBlend),
                                                    .dstAlphaBlendFactor = VkBlendFactor(i.dstAlphaBlend),
                                                    .alphaBlendOp = VkBlendOp(i.alphaBlendOp),
                                                    .colorWriteMask = i.colorWriteMask & 0xf};
    const VkPipelineColorBlendStateCreateInfo blending{.sType =
                                                           VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                       .attachmentCount = depthOnly ? 0u : 1u,
                                                       .pAttachments = &blend};
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic{.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                   .dynamicStateCount = 2,
                                                   .pDynamicStates = dynamicStates};
    const VkGraphicsPipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                            .stageCount = 2,
                                            .pStages = stages,
                                            .pVertexInputState = &input,
                                            .pInputAssemblyState = &assembly,
                                            .pViewportState = &viewport,
                                            .pRasterizationState = &raster,
                                            .pMultisampleState = &samples,
                                            .pDepthStencilState = &depth,
                                            .pColorBlendState = &blending,
                                            .pDynamicState = &dynamic,
                                            .layout = layout.pipeline,
                                            .renderPass = renderPass};
    VkPipeline result = VK_NULL_HANDLE;
    const auto status = pipeline::createGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &result);
    if (status == VK_NOT_READY)
      return VK_NULL_HANDLE; // compiling: this frame skips the pass
    check(status);
    m_pipelines.emplace(std::move(key), result);
    std::lock_guard lock(m_mutex);
    m_stats.pipelines = uint32_t(m_pipelines.size());
    return result;
  }

  void HostPasses::reserve(Slot &slot, VkDeviceSize bytes) {
    if (slot.buffer && slot.size >= bytes)
      return;
    if (slot.mapped)
      vkUnmapMemory(m_device, slot.memory);
    if (slot.buffer)
      vkDestroyBuffer(m_device, slot.buffer, nullptr);
    if (slot.memory)
      vkFreeMemory(m_device, slot.memory, nullptr);
    slot.buffer = VK_NULL_HANDLE;
    slot.memory = VK_NULL_HANDLE;
    slot.mapped = nullptr;
    slot.size = std::max(bytes + bytes / 2, kSlotMinimum);
    const VkBufferCreateInfo buffer{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = slot.size,
                                    .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    check(vkCreateBuffer(m_device, &buffer, nullptr, &slot.buffer));
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(m_device, slot.buffer, &requirements);
    const VkMemoryAllocateInfo allocation{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType(requirements.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    check(vkAllocateMemory(m_device, &allocation, nullptr, &slot.memory));
    check(vkBindBufferMemory(m_device, slot.buffer, slot.memory, 0));
    void *mapped = nullptr;
    check(vkMapMemory(m_device, slot.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    slot.mapped = static_cast<uint8_t *>(mapped);
  }

  VkDeviceSize HostPasses::stage(Slot &slot, const void *data, VkDeviceSize bytes, VkDeviceSize alignment) {
    const VkDeviceSize offset = (slot.used + alignment - 1) / alignment * alignment;
    if (offset + bytes > slot.size)
      throw std::runtime_error("host pass staging overflow");
    std::memcpy(slot.mapped + offset, data, bytes);
    slot.used = offset + bytes;
    return offset;
  }

  void HostPasses::barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                           VkAccessFlags source, VkAccessFlags destination, VkPipelineStageFlags sourceStage,
                           VkPipelineStageFlags destinationStage) {
    const VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                 .srcAccessMask = source,
                                 .dstAccessMask = destination,
                                 .oldLayout = from,
                                 .newLayout = to,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = image,
                                 .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &b);
  }

  void HostPasses::blit(VkCommandBuffer command, VkImage source, VkExtent2D sourceExtent, const int32_t sourceRect[4],
                        VkImage destination, VkExtent2D destinationExtent, const int32_t targetRect[4], bool linear) {
    auto offsets = [](const int32_t rect[4], VkExtent2D extent, VkOffset3D out[2]) {
      if (rect[2] == 0) {
        out[0] = {0, 0, 0};
        out[1] = {int32_t(extent.width), int32_t(extent.height), 1};
      } else {
        out[0] = {rect[0], rect[1], 0};
        out[1] = {rect[0] + rect[2], rect[1] + rect[3], 1};
      }
    };
    VkImageBlit region{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                       .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}};
    offsets(sourceRect, sourceExtent, region.srcOffsets);
    offsets(targetRect, destinationExtent, region.dstOffsets);
    vkCmdBlitImage(command, source, VK_IMAGE_LAYOUT_GENERAL, destination, VK_IMAGE_LAYOUT_GENERAL, 1, &region,
                   linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  }

  void HostPasses::record(VkCommandBuffer command, uint32_t slotIndex, VkImage image, VkFormat format,
                          VkExtent2D extent, VkImageLayout imageLayout, VkImageView viewDepth) {
    std::vector<Pass> passes;
    std::vector<Upload> uploads;
    {
      std::lock_guard lock(m_mutex);
      // The host's latest list, kept for the frames that come before its next
      // one (presentation runs on its own thread: a frame recorded between two
      // submissions drew none of the passes, and the effect flickered). The
      // host submits every frame, an empty list when it has no passes.
      passes = m_pending;
      uploads = std::move(m_uploads);
      m_uploads.clear();
      m_stats.passes = uint32_t(passes.size());
    }
    if (passes.empty() && uploads.empty())
      return;
    ++m_frame;
    constexpr VkAccessFlags all = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    constexpr VkPipelineStageFlags everything = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    auto &slot = m_slots[slotIndex];
    // The frame's staging: every pass's bytes, aligned for uniform buffers.
    VkDeviceSize bytes = 0;
    for (const auto &pass : passes)
      bytes +=
          pass.data.size() + pass.indices.size() * 4 + 256 * (pass.buffers.size() + pass.vertexBindings.size() + 2);
    for (const auto &upload : uploads)
      bytes += upload.pixels.size() + 256;
    reserve(slot, bytes);
    slot.used = 0;
    // The host's texture contents first: into their targets, through staging.
    for (const auto &upload : uploads) {
      auto &t = target(upload.target, VkFormat(upload.format), {upload.width, upload.height}, 0, upload.depth);
      barrier(command, t.image, t.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_GENERAL, all, VK_ACCESS_TRANSFER_WRITE_BIT, everything, VK_PIPELINE_STAGE_TRANSFER_BIT);
      t.initialized = true;
      const VkBufferImageCopy region{.bufferOffset = stage(slot, upload.pixels.data(), upload.pixels.size(), 16),
                                     .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .imageExtent = {upload.width, upload.height, upload.depth}};
      vkCmdCopyBufferToImage(command, slot.buffer, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }
    if (!slot.pool) {
      const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2048},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048},
                                            {VK_DESCRIPTOR_TYPE_SAMPLER, 2048}};
      const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = 1024,
                                            .poolSizeCount = 4,
                                            .pPoolSizes = sizes};
      check(vkCreateDescriptorPool(m_device, &pool, nullptr, &slot.pool));
    }
    check(vkResetDescriptorPool(m_device, slot.pool, 0));
    // The frame, raw, in the swapchain's channel order.
    const VkFormat frameFormat = unorm(format);
    auto &frame = target(0, frameFormat, extent, 0);
    barrier(command, image, imageLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all, VK_ACCESS_TRANSFER_READ_BIT,
            everything, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(command, frame.image, frame.initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, all, VK_ACCESS_TRANSFER_WRITE_BIT, everything, VK_PIPELINE_STAGE_TRANSFER_BIT);
    frame.initialized = true;
    const VkImageCopy copy{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .extent = {extent.width, extent.height, 1}};
    vkCmdCopyImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                   &copy);
    bool frameWritten = false;
    auto between = [&] { // every image of this module is in GENERAL layout: one barrier covers any order
      const VkMemoryBarrier memory{
          .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = all, .dstAccessMask = all};
      vkCmdPipelineBarrier(command, everything, everything, 0, 1, &memory, 0, nullptr, 0, nullptr);
    };
    auto skip = [&](uint32_t &reason) {
      ++m_stats.skipped;
      ++reason;
    };
    auto ensure = [&](remixapi_HostTargetHandleEXT handle, const remixapi_HostPassEXT &info) -> Target & {
      if (!handle)
        return frame;
      auto &t = target(handle, info.targetFormat ? VkFormat(info.targetFormat) : frameFormat,
                       {info.targetWidth, info.targetHeight}, info.targetChannels);
      if (!t.initialized) {
        barrier(command, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, all, everything, everything);
        t.initialized = true;
      }
      return t;
    };
    for (const auto &pass : passes) {
      const auto &i = pass.info;
      between();
      if (i.kind == REMIXAPI_HOST_PASS_CLEAR_EXT && i.target == REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT) {
        auto &t = frameDepth(command, extent);
        const VkClearDepthStencilValue value{i.clearColor[0], 0};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(command, t.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        ++m_stats.clears;
        continue;
      }
      if (i.kind == REMIXAPI_HOST_PASS_CLEAR_EXT) {
        auto &t = ensure(i.target, i);
        const VkClearColorValue color{{i.clearColor[0], i.clearColor[1], i.clearColor[2], i.clearColor[3]}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, t.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
        frameWritten |= !i.target;
        ++m_stats.clears;
        continue;
      }
      if (i.kind == REMIXAPI_HOST_PASS_COPY_EXT) {
        auto &t = ensure(i.target, i);
        auto source = m_targets.find(i.source);
        if (source == m_targets.end() || !source->second.initialized || !fits(i.sourceRect, source->second.extent) ||
            !fits(i.targetRect, t.extent)) {
          skip(m_stats.skippedCopySource);
          continue;
        }
        source->second.lastFrame = m_frame;
        blit(command, source->second.image, source->second.extent, i.sourceRect, t.image, t.extent, i.targetRect,
             i.linear != 0);
        frameWritten |= !i.target;
        ++m_stats.copies;
        continue;
      }
      // A draw.
      std::shared_ptr<Shader> vertex, fragment;
      {
        std::lock_guard lock(m_mutex);
        auto v = m_shaders.find(i.vertexShader), f = m_shaders.find(i.fragmentShader);
        if (v != m_shaders.end() && f != m_shaders.end()) {
          vertex = v->second;
          fragment = f->second;
        }
      }
      if (!vertex || !fragment || vertex->stage != VK_SHADER_STAGE_VERTEX_BIT ||
          fragment->stage != VK_SHADER_STAGE_FRAGMENT_BIT) {
        skip(m_stats.skippedShaders);
        continue;
      }
      const bool depthOnly = i.target == REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT;
      auto &t = depthOnly ? frameDepth(command, extent) : ensure(i.target, i);
      if (!t.pass) {
        skip(m_stats.skippedTarget); // a volume: sampled only
        continue;
      }
      VkRenderPass renderPass = t.pass;
      VkFramebuffer framebuffer = t.framebuffer;
      if (!i.target && i.depth) {
        // A depth-tested draw on the frame: against the host's frame depth.
        auto depth = m_targets.find(REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT);
        if (depth == m_targets.end() || !depth->second.initialized) {
          skip(m_stats.skippedTarget);
          continue;
        }
        depth->second.lastFrame = m_frame;
        attachDepth(t, depth->second);
        renderPass = t.depthPass;
        framebuffer = t.depthFramebuffer;
      }
      const auto &l = layout(*vertex, *fragment);
      const VkPipeline p = pipeline(pass, *vertex, *fragment, l, renderPass, depthOnly);
      if (!p) {
        skip(m_stats.skippedPipeline);
        continue;
      }
      // Descriptors: a buffer or sampler for each binding the layout has; the
      // frame and the host's targets as sampled images (GENERAL layout).
      const VkDescriptorSetAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                   .descriptorPool = slot.pool,
                                                   .descriptorSetCount = 1,
                                                   .pSetLayouts = &l.set};
      VkDescriptorSet set = VK_NULL_HANDLE;
      if (vkAllocateDescriptorSets(m_device, &allocation, &set) != VK_SUCCESS) {
        skip(m_stats.skippedDescriptors);
        continue;
      }
      std::vector<VkWriteDescriptorSet> writes;
      std::vector<VkDescriptorBufferInfo> bufferInfos;
      std::vector<VkDescriptorImageInfo> imageInfos;
      bufferInfos.reserve(l.bindings.size());
      imageInfos.reserve(l.bindings.size());
      size_t dataOffset = 0;
      std::vector<VkDeviceSize> bufferOffsets(pass.buffers.size());
      for (size_t b = 0; b < pass.buffers.size(); ++b) {
        bufferOffsets[b] = stage(slot, pass.data.data() + dataOffset, pass.buffers[b].bytes, 256);
        dataOffset += pass.buffers[b].bytes;
      }
      const uint8_t *push = pass.data.data() + dataOffset;
      dataOffset += i.pushConstantBytes;
      std::vector<VkDeviceSize> vertexOffsets(pass.vertexBindings.size());
      for (size_t v = 0; v < pass.vertexBindings.size(); ++v) {
        vertexOffsets[v] = stage(slot, pass.data.data() + dataOffset, pass.vertexBindings[v].bytes, 16);
        dataOffset += pass.vertexBindings[v].bytes;
      }
      const VkDeviceSize indexOffset = stage(slot, pass.indices.data(), pass.indices.size() * 4, 4);
      bool complete = true;
      uint32_t *incomplete = &m_stats.skippedBuffer;
      for (const auto &binding : l.bindings) {
        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                   .dstSet = set,
                                   .dstBinding = binding.binding,
                                   .descriptorCount = 1};
        if (binding.type == REMIXAPI_HOST_BINDING_UNIFORM_BUFFER_EXT) {
          size_t found = pass.buffers.size();
          for (size_t b = 0; b < pass.buffers.size(); ++b)
            if (pass.buffers[b].binding == binding.binding)
              found = b;
          if (found == pass.buffers.size()) {
            complete = false;
            incomplete = &m_stats.skippedBuffer;
            break;
          }
          bufferInfos.push_back({slot.buffer, bufferOffsets[found], pass.buffers[found].bytes});
          write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
          write.pBufferInfo = &bufferInfos.back();
        } else {
          const remixapi_HostPassSamplerEXT *found = nullptr;
          for (const auto &s : pass.samplers)
            if (s.binding == binding.binding)
              found = &s;
          if (!found) {
            complete = false;
            incomplete = &m_stats.skippedSampler;
            break;
          }
          // A D3D11 sampler state (SAMPLER_STATE) names no image; a D3D11
          // texture (SAMPLED_IMAGE) no filtering; a D3D9 sampler (SAMPLER) both.
          const bool image = binding.type != REMIXAPI_HOST_BINDING_SAMPLER_STATE_EXT;
          const bool filtering = binding.type != REMIXAPI_HOST_BINDING_SAMPLED_IMAGE_EXT;
          VkImageView sampled = VK_NULL_HANDLE;
          if (image) {
            sampled = frame.sampled;
            if (found->target == REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT) {
              // The runtime's own image, when the frame has it.
              if (!viewDepth) {
                complete = false;
                incomplete = &m_stats.skippedViewDepth;
                break;
              }
              sampled = viewDepth;
            } else if (found->target) {
              auto it = m_targets.find(found->target);
              if (it == m_targets.end() || !it->second.initialized) {
                complete = false;
                incomplete = &m_stats.skippedTarget;
                break;
              }
              sampled = it->second.sampled;
              it->second.lastFrame = m_frame;
            } else if (!i.target) {
              // Sampling the frame while drawing it: through a copy of it.
              auto &scratch = target(~0ull, frameFormat, extent, 0);
              if (!scratch.initialized) {
                barrier(command, scratch.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, all, everything,
                        everything);
                scratch.initialized = true;
              }
              const int32_t whole[4] = {};
              blit(command, frame.image, extent, whole, scratch.image, extent, whole, false);
              between();
              sampled = scratch.sampled;
            }
          }
          const VkSamplerAddressMode modes[] = {VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
          const VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                                .magFilter = found->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                                .minFilter = found->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                                .addressModeU = modes[found->addressU],
                                                .addressModeV = modes[found->addressV],
                                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                .maxLod = VK_LOD_CLAMP_NONE,
                                                .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK};
          VkSampler sampler = VK_NULL_HANDLE;
          if (filtering)
            check(pipeline::createSampler(m_device, &samplerInfo, nullptr, &sampler));
          imageInfos.push_back({sampler, sampled, image ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED});
          write.descriptorType = descriptorType(binding.type);
          write.pImageInfo = &imageInfos.back();
        }
        writes.push_back(write);
      }
      if (!complete) {
        skip(*incomplete);
        continue;
      }
      vkUpdateDescriptorSets(m_device, uint32_t(writes.size()), writes.data(), 0, nullptr);
      const VkRenderPassBeginInfo begin{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                        .renderPass = renderPass,
                                        .framebuffer = framebuffer,
                                        .renderArea = {{0, 0}, t.extent}};
      vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
      // D3D9's viewport: y down (a negative height flips it) with pixel
      // centres half a pixel before Vulkan's, so the host's geometry shifts by
      // half a pixel (a little less, as DXVK does, to keep edges off centres).
      const float vx = i.viewport[2] ? float(i.viewport[0]) : 0.f, vy = i.viewport[2] ? float(i.viewport[1]) : 0.f;
      const float vw = i.viewport[2] ? float(i.viewport[2]) : float(t.extent.width);
      const float vh = i.viewport[2] ? float(i.viewport[3]) : float(t.extent.height);
      constexpr float half = .5f - 1.f / 128;
      const VkViewport viewport{vx + half, vy + vh + half, vw, -vh, 0, 1};
      vkCmdSetViewport(command, 0, 1, &viewport);
      VkRect2D scissor{{0, 0}, t.extent};
      if (i.scissor[2] && fits(i.scissor, t.extent))
        scissor = {{i.scissor[0], i.scissor[1]}, {uint32_t(i.scissor[2]), uint32_t(i.scissor[3])}};
      vkCmdSetScissor(command, 0, 1, &scissor);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, l.pipeline, 0, 1, &set, 0, nullptr);
      if (l.pushBytes) {
        uint8_t block[128] = {};
        std::memcpy(block, push, std::min<uint32_t>(i.pushConstantBytes, l.pushBytes));
        vkCmdPushConstants(command, l.pipeline, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           l.pushBytes, block);
      }
      for (uint32_t v = 0; v < pass.vertexBindings.size(); ++v)
        vkCmdBindVertexBuffers(command, v, 1, &slot.buffer, &vertexOffsets[v]);
      vkCmdBindIndexBuffer(command, slot.buffer, indexOffset, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(command, uint32_t(pass.indices.size()), 1, 0, 0, 0);
      vkCmdEndRenderPass(command);
      frameWritten |= !i.target;
      ++m_stats.draws;
    }
    between();
    // The frame back, if a pass wrote it; the swapchain image to its layout.
    if (frameWritten) {
      barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT);
      vkCmdCopyImage(command, frame.image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &copy);
      barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, imageLayout, VK_ACCESS_TRANSFER_WRITE_BIT, all,
              VK_PIPELINE_STAGE_TRANSFER_BIT, everything);
    } else {
      barrier(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imageLayout, VK_ACCESS_TRANSFER_READ_BIT, all,
              VK_PIPELINE_STAGE_TRANSFER_BIT, everything);
    }
    // Replaced targets once no frame in flight can use them (each recorded
    // frame waits for its slot's previous one).
    for (auto it = m_retired.begin(); it != m_retired.end();) {
      if (it->first + gpuFrameCount + 1 <= m_frame) {
        destroyTarget(it->second);
        it = m_retired.erase(it);
      } else
        ++it;
    }
    // Targets the host stopped using go after a while.
    for (auto it = m_targets.begin(); it != m_targets.end();) {
      if (it->first && it->first < REMIXAPI_HOST_TARGET_RESERVED_EXT && it->second.lastFrame + 600 < m_frame) {
        destroyTarget(it->second);
        it = m_targets.erase(it);
      } else
        ++it;
    }
    std::lock_guard lock(m_mutex);
    m_stats.targets = uint32_t(m_targets.size());
  }
#endif
} // namespace remix
