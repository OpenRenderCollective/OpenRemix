#include "sky/bridge.h"
#include "platform/memory_release.h"
#include "runtime/pipelines.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace remix::sky {
  namespace {
    constexpr size_t constantRingBytes = 4u << 20;
    constexpr size_t stagingChunkBytes = 8u << 20;
    constexpr uint32_t setsPerPool = 256;

    void check(VkResult result) {
      if (result != VK_SUCCESS)
        throw result;
    }

    VkFormat format(numos::Format value) {
      switch (value) {
      case numos::Format::R8Unorm: return VK_FORMAT_R8_UNORM;
      case numos::Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
      case numos::Format::R16Float: return VK_FORMAT_R16_SFLOAT;
      case numos::Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
      case numos::Format::R32Uint: return VK_FORMAT_R32_UINT;
      case numos::Format::R32Float: return VK_FORMAT_R32_SFLOAT;
      case numos::Format::RG32Float: return VK_FORMAT_R32G32_SFLOAT;
      case numos::Format::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
      case numos::Format::RG16Float: return VK_FORMAT_R16G16_SFLOAT;
      }
      return VK_FORMAT_UNDEFINED;
    }

    size_t texelBytes(numos::Format value) {
      switch (value) {
      case numos::Format::R8Unorm: return 1;
      case numos::Format::R16Float: return 2;
      case numos::Format::RGBA8Unorm:
      case numos::Format::R32Uint:
      case numos::Format::R32Float:
      case numos::Format::RG16Float: return 4;
      case numos::Format::RGBA16Float:
      case numos::Format::RG32Float: return 8;
      case numos::Format::RGBA32Float: return 16;
      }
      return 4;
    }

    // The numos::Format of a storage image's SPIR-V ImageFormat (placeholders).
    numos::Format fromSpirv(uint32_t value) {
      switch (value) {
      case 1: return numos::Format::RGBA32Float;  // Rgba32f
      case 2: return numos::Format::RGBA16Float;  // Rgba16f
      case 3: return numos::Format::R32Float;     // R32f
      case 4: return numos::Format::RGBA8Unorm;   // Rgba8
      case 6: return numos::Format::RG32Float;    // Rg32f
      case 7: return numos::Format::RG16Float;    // Rg16f
      case 9: return numos::Format::R16Float;     // R16f
      case 15: return numos::Format::R8Unorm;     // R8
      case 33: return numos::Format::R32Uint;     // R32ui
      }
      return numos::Format::RGBA16Float;
    }

    VkDescriptorType descriptorType(numos::BindingKind kind) {
      switch (kind) {
      case numos::BindingKind::ConstantBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      case numos::BindingKind::SampledTexture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      case numos::BindingKind::StorageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      case numos::BindingKind::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
      case numos::BindingKind::CombinedTextureSampler: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      case numos::BindingKind::StructuredBuffer:
      case numos::BindingKind::RWStructuredBuffer: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      case numos::BindingKind::AccelerationStructure: return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      }
      return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    }

    VkImageViewType viewType(numos::TextureType type) {
      return type == numos::TextureType::Tex3D        ? VK_IMAGE_VIEW_TYPE_3D
             : type == numos::TextureType::Tex2DArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                     : VK_IMAGE_VIEW_TYPE_2D;
    }

    uint32_t layers(const numos::TextureDesc &desc) {
      return desc.type == numos::TextureType::Tex2DArray ? desc.depth : 1;
    }

    constexpr VkPipelineStageFlags workStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    constexpr VkAccessFlags workAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                         VK_ACCESS_UNIFORM_READ_BIT;

    void memoryBarrier(VkCommandBuffer command, VkPipelineStageFlags source, VkAccessFlags writes,
                       VkPipelineStageFlags destination, VkAccessFlags reads) {
      const VkMemoryBarrier barrier{
          .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = writes, .dstAccessMask = reads};
      vkCmdPipelineBarrier(command, source, destination, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }
  } // namespace

  Bridge::Bridge(VkPhysicalDevice physical, VkDevice device, std::vector<HostPass> hostPasses)
      : m_device(device) {
    vkGetPhysicalDeviceProperties(physical, &m_properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &m_memory);
    for (uint32_t i = 0; i < numos::passCount(); ++i)
      m_passes.push_back({.info = numos::passInfo(i), .code = numosShader(i)});
    for (auto &pass : hostPasses)
      m_passes.push_back({.info = pass.info, .code = pass.code});
    for (auto &pass : m_passes) {
      std::vector<VkDescriptorSetLayoutBinding> bindings;
      for (uint32_t i = 0; i < pass.info.bindingCount; ++i) {
        const auto &binding = pass.info.bindings[i];
        if (binding.set != 0)
          throw std::runtime_error("NUMOS pass binds a descriptor set other than 0");
        bindings.push_back({.binding = binding.slot,
                            .descriptorType = descriptorType(binding.kind),
                            .descriptorCount = 1,
                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT});
      }
      const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                    .bindingCount = uint32_t(bindings.size()),
                                                    .pBindings = bindings.data()};
      check(pipeline::createDescriptorSetLayout(device, &setInfo, nullptr, &pass.setLayout));
      const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pass.info.pushConstantSize};
      const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                  .setLayoutCount = 1,
                                                  .pSetLayouts = &pass.setLayout,
                                                  .pushConstantRangeCount = pass.info.pushConstantSize ? 1u : 0u,
                                                  .pPushConstantRanges = &push};
      check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &pass.layout));
    }
    for (auto &slot : m_slots) {
      Resource ring{.kind = Resource::Kind::Buffer};
      ring.imported = true;
      ring.buffer = {.usage = numos::BufferUsage::Constant, .size = constantRingBytes};
      ring.handle = makeBuffer(constantRingBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, ring.memory, &ring.mapped);
      slot.constants = add(std::move(ring));
    }
  }

  Bridge::~Bridge() {
    for (auto &slot : m_slots) {
      for (auto &resource : slot.retired)
        release(resource);
      for (auto &staging : slot.staging) {
        vkDestroyBuffer(m_device, staging.buffer, nullptr);
        MemoryRelease::free(m_device, staging.memory);
      }
      for (auto pool : slot.pools)
        vkDestroyDescriptorPool(m_device, pool, nullptr);
    }
    for (auto &[handle, resource] : m_resources) {
      if (resource.imported && resource.kind != Resource::Kind::Buffer)
        continue;
      resource.imported = false;
      release(resource);
    }
    for (auto &pass : m_passes) {
      if (pass.pipeline)
        pipeline::destroyPipeline(m_device, pass.pipeline, nullptr);
      if (pass.module)
        pipeline::destroyShaderModule(m_device, pass.module, nullptr);
      pipeline::destroyPipelineLayout(m_device, pass.layout, nullptr);
      pipeline::destroyDescriptorSetLayout(m_device, pass.setLayout, nullptr);
    }
  }

  numos::GpuHandle Bridge::add(Resource resource) {
    const auto handle = m_next++;
    m_resources.emplace(handle, std::move(resource));
    return handle;
  }

  Bridge::Resource &Bridge::get(numos::GpuHandle handle) {
    auto found = m_resources.find(handle);
    if (found == m_resources.end())
      throw std::runtime_error("Unknown NUMOS GPU handle");
    return found->second;
  }

  const Bridge::Resource &Bridge::get(numos::GpuHandle handle) const {
    auto found = m_resources.find(handle);
    if (found == m_resources.end())
      throw std::runtime_error("Unknown NUMOS GPU handle");
    return found->second;
  }

  void Bridge::release(Resource &resource) {
    if (resource.imported)
      return;
    for (auto &[key, view] : resource.views)
      vkDestroyImageView(m_device, view, nullptr);
    if (resource.image)
      vkDestroyImage(m_device, resource.image, nullptr);
    if (resource.handle)
      vkDestroyBuffer(m_device, resource.handle, nullptr);
    if (resource.sampler)
      vkDestroySampler(m_device, resource.sampler, nullptr);
    MemoryRelease::free(m_device, resource.memory);
    resource = {};
  }

  uint32_t Bridge::memoryType(uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const {
    for (const auto flags : {required | preferred, required})
      for (uint32_t i = 0; i < m_memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m_memory.memoryTypes[i].propertyFlags & flags) == flags)
          return i;
    throw VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }

  VkBuffer Bridge::makeBuffer(size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                              VkMemoryPropertyFlags preferred, VkDeviceMemory &memory, void **mapped) {
    const VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = std::max<size_t>(size, 16),
                                  .usage = usage,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer = VK_NULL_HANDLE;
    check(vkCreateBuffer(m_device, &info, nullptr, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(m_device, buffer, &requirements);
    const VkMemoryAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                        .allocationSize = requirements.size,
                                        .memoryTypeIndex = memoryType(requirements.memoryTypeBits, required, preferred)};
    if (VkResult result = vkAllocateMemory(m_device, &allocate, nullptr, &memory); result != VK_SUCCESS) {
      vkDestroyBuffer(m_device, buffer, nullptr);
      throw result;
    }
    check(vkBindBufferMemory(m_device, buffer, memory, 0));
    if (mapped)
      check(vkMapMemory(m_device, memory, 0, VK_WHOLE_SIZE, 0, mapped));
    return buffer;
  }

  void Bridge::selectFrame(uint32_t slot) {
    m_slot = slot % gpuFrameCount;
    ++m_frame;
    auto &s = m_slots[m_slot];
    for (auto &resource : s.retired)
      release(resource);
    s.retired.clear();
    s.constantsUsed = 0;
    for (auto &staging : s.staging)
      staging.used = 0;
    for (auto pool : s.pools)
      vkResetDescriptorPool(m_device, pool, 0);
    s.pool = 0;
  }

  bool Bridge::preparePipelines(const std::function<bool(uint32_t pass)> &needed, uint32_t &ready,
                                uint32_t &total) {
    ready = total = 0;
    for (uint32_t index = 0; index < m_passes.size(); ++index) {
      auto &pass = m_passes[index];
      if (!needed(index))
        continue;
      ++total;
      if (!pass.pipeline) {
        if (!pass.module) {
          const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                    .codeSize = pass.code.bytes,
                                                    .pCode = pass.code.code};
          check(pipeline::createShaderModule(m_device, &moduleInfo, nullptr, &pass.module));
        }
        const VkComputePipelineCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = pass.module,
                      .pName = "main"},
            .layout = pass.layout};
        const auto result = pipeline::createComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &pass.pipeline);
        if (result != VK_SUCCESS && result != VK_NOT_READY)
          throw result;
        if (pass.pipeline) {
          pipeline::destroyShaderModule(m_device, pass.module, nullptr);
          pass.module = VK_NULL_HANDLE;
        }
      }
      ready += pass.pipeline != VK_NULL_HANDLE;
    }
    return ready == total;
  }

  void Bridge::begin(VkCommandBuffer command) {
    m_command = command;
    // The host's TLAS build and depth writes come before NUMOS reads them.
    memoryBarrier(command,
                  VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | workStages,
                  VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | workAccess,
                  workStages, workAccess | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
  }

  void Bridge::end() {
    while (!m_zones.empty())
      m_zones.pop_back();
    memoryBarrier(m_command, workStages, workAccess,
                  workStages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT);
    m_command = VK_NULL_HANDLE;
  }

  VkImage Bridge::image(numos::GpuHandle texture) const { return get(texture).image; }

  VkImageView Bridge::sampledView(numos::GpuHandle texture) {
    auto &resource = get(texture);
    return view(resource, 0, ~0u, false);
  }

  VkSampler Bridge::sampler(numos::GpuHandle sampler) const { return get(sampler).sampler; }

  numos::GpuHandle Bridge::importTexture(VkImage image, VkImageView view, const numos::TextureDesc &desc) {
    Resource resource{.kind = Resource::Kind::Texture, .imported = true, .texture = desc, .image = image};
    resource.importedView = view;
    return add(std::move(resource));
  }

  numos::GpuHandle Bridge::importAccelerationStructure(VkAccelerationStructureKHR accel) {
    Resource resource{.kind = Resource::Kind::Accel, .imported = true};
    resource.accel = accel;
    return add(std::move(resource));
  }

  void Bridge::forget(numos::GpuHandle handle) {
    auto found = m_resources.find(handle);
    if (found != m_resources.end() && found->second.imported)
      m_resources.erase(found);
  }

  VkImageView Bridge::view(Resource &texture, uint32_t mip, uint32_t count, bool storage) {
    if (texture.importedView)
      return texture.importedView;
    const uint32_t levels = std::max(texture.texture.mipLevels, 1u);
    mip = std::min(mip, levels - 1);
    count = storage ? 1 : std::min(count, levels - mip);
    const uint64_t key = uint64_t(mip) | uint64_t(count) << 16 | uint64_t(storage) << 32;
    if (auto found = texture.views.find(key); found != texture.views.end())
      return found->second;
    const VkImageViewCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture.image,
        .viewType = viewType(texture.texture.type),
        .format = format(texture.texture.format),
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, count, 0, layers(texture.texture)}};
    VkImageView view = VK_NULL_HANDLE;
    check(vkCreateImageView(m_device, &info, nullptr, &view));
    texture.views.emplace(key, view);
    return view;
  }

  numos::GpuHandle Bridge::createTexture(const numos::TextureDesc &desc) {
    Resource resource{.kind = Resource::Kind::Texture, .texture = desc};
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = desc.type == numos::TextureType::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = format(desc.format),
        .extent = {std::max(desc.width, 1u), std::max(desc.height, 1u),
                   desc.type == numos::TextureType::Tex3D ? std::max(desc.depth, 1u) : 1u},
        .mipLevels = std::max(desc.mipLevels, 1u),
        .arrayLayers = layers(desc),
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    check(vkCreateImage(m_device, &info, nullptr, &resource.image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(m_device, resource.image, &requirements);
    const VkMemoryAllocateInfo allocate{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (VkResult result = vkAllocateMemory(m_device, &allocate, nullptr, &resource.memory); result != VK_SUCCESS) {
      vkDestroyImage(m_device, resource.image, nullptr);
      throw result;
    }
    check(vkBindImageMemory(m_device, resource.image, resource.memory, 0));
    if (!m_command)
      throw std::runtime_error("NUMOS created a texture outside a frame");
    const VkImageMemoryBarrier toGeneral{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = workAccess,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = resource.image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, workStages, 0, 0, nullptr, 0, nullptr, 1,
                         &toGeneral);
    return add(std::move(resource));
  }

  numos::GpuHandle Bridge::createBuffer(const numos::BufferDesc &desc) {
    Resource resource{.kind = Resource::Kind::Buffer, .buffer = desc};
    switch (desc.usage) {
    case numos::BufferUsage::Constant:
      resource.handle = makeBuffer(desc.size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, resource.memory, nullptr);
      break;
    case numos::BufferUsage::Structured:
      resource.handle = makeBuffer(desc.size,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, resource.memory, nullptr);
      break;
    case numos::BufferUsage::Readback:
      resource.handle = makeBuffer(desc.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   VK_MEMORY_PROPERTY_HOST_CACHED_BIT, resource.memory, &resource.mapped);
      break;
    }
    return add(std::move(resource));
  }

  numos::GpuHandle Bridge::createSampler(const numos::SamplerDesc &desc) {
    const auto address = [](numos::AddressMode mode) {
      return mode == numos::AddressMode::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    };
    const auto filter = [](numos::Filter value) {
      return value == numos::Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    };
    const VkSamplerCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = filter(desc.filter),
        .minFilter = filter(desc.filter),
        .mipmapMode = desc.mipFilter == numos::Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                             : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = address(desc.addressU),
        .addressModeV = address(desc.addressV),
        .addressModeW = address(desc.addressW),
        .maxLod = desc.maxLod >= numos::SamplerDesc::kLodClampNone ? VK_LOD_CLAMP_NONE : desc.maxLod};
    Resource resource{.kind = Resource::Kind::Sampler};
    check(vkCreateSampler(m_device, &info, nullptr, &resource.sampler));
    return add(std::move(resource));
  }

  void Bridge::destroy(numos::GpuHandle handle) {
    auto found = m_resources.find(handle);
    if (found == m_resources.end())
      return;
    m_slots[m_slot].retired.push_back(std::move(found->second));
    m_resources.erase(found);
  }

  numos::IGpu::ConstantsRegion Bridge::uploadConstants(const void *data, size_t size) {
    auto &slot = m_slots[m_slot];
    auto &ring = get(slot.constants);
    const size_t alignment = std::max<size_t>(m_properties.limits.minUniformBufferOffsetAlignment, 16);
    const size_t offset = (slot.constantsUsed + alignment - 1) / alignment * alignment;
    if (offset + size > constantRingBytes)
      throw std::runtime_error("NUMOS constants exceed the frame's ring");
    std::memcpy(static_cast<uint8_t *>(ring.mapped) + offset, data, size);
    slot.constantsUsed = offset + size;
    return {slot.constants, offset, size};
  }

  std::pair<Bridge::Staging *, size_t> Bridge::stage(const void *data, size_t size) {
    auto &slot = m_slots[m_slot];
    Staging *target = nullptr;
    for (auto &staging : slot.staging)
      if (staging.used + size <= staging.size) {
        target = &staging;
        break;
      }
    if (!target) {
      Staging staging;
      staging.size = std::max(size, stagingChunkBytes);
      void *mapped = nullptr;
      staging.buffer = makeBuffer(staging.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0,
                                  staging.memory, &mapped);
      staging.mapped = static_cast<uint8_t *>(mapped);
      slot.staging.push_back(staging);
      target = &slot.staging.back();
    }
    const size_t offset = target->used;
    std::memcpy(target->mapped + offset, data, size);
    target->used = (offset + size + 15) & ~size_t(15);
    return {target, offset};
  }

  void Bridge::transferBarrier() { memoryBarrier(m_command, workStages, workAccess, workStages, workAccess); }

  void Bridge::updateBuffer(numos::GpuHandle buffer, size_t offset, size_t size, const void *data) {
    if (!size)
      return;
    auto [staging, source] = stage(data, size);
    transferBarrier();
    const VkBufferCopy region{source, offset, size};
    vkCmdCopyBuffer(m_command, staging->buffer, get(buffer).handle, 1, &region);
    transferBarrier();
  }

  void Bridge::copyBuffer(numos::GpuHandle dst, size_t dstOffset, numos::GpuHandle src, size_t srcOffset, size_t size) {
    auto &target = get(dst);
    transferBarrier();
    const VkBufferCopy region{srcOffset, dstOffset, size};
    vkCmdCopyBuffer(m_command, get(src).handle, target.handle, 1, &region);
    transferBarrier();
    if (target.buffer.usage == numos::BufferUsage::Readback) {
      target.copyFrame = m_frame;
      // The host reads mapped memory once the slot's fence has passed.
      memoryBarrier(m_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }
  }

  void Bridge::copyTexture(numos::GpuHandle dst, numos::GpuHandle src) {
    const auto &source = get(src), &target = get(dst);
    std::vector<VkImageCopy> regions;
    const auto &desc = target.texture;
    for (uint32_t mip = 0; mip < std::max(desc.mipLevels, 1u); ++mip)
      regions.push_back({.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layers(desc)},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layers(desc)},
                         .extent = {std::max(desc.width >> mip, 1u), std::max(desc.height >> mip, 1u),
                                    desc.type == numos::TextureType::Tex3D ? std::max(desc.depth >> mip, 1u) : 1u}});
    transferBarrier();
    vkCmdCopyImage(m_command, source.image, VK_IMAGE_LAYOUT_GENERAL, target.image, VK_IMAGE_LAYOUT_GENERAL,
                   uint32_t(regions.size()), regions.data());
    transferBarrier();
  }

  void Bridge::clearTexture(numos::GpuHandle texture, const float rgba[4]) {
    const auto &target = get(texture);
    VkClearColorValue value{};
    if (target.texture.format == numos::Format::R32Uint)
      for (int i = 0; i < 4; ++i)
        value.uint32[i] = uint32_t(rgba[i]);
    else
      std::copy_n(rgba, 4, value.float32);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                        VK_REMAINING_ARRAY_LAYERS};
    transferBarrier();
    vkCmdClearColorImage(m_command, target.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
    transferBarrier();
  }

  void Bridge::uploadTexture(numos::GpuHandle texture, uint32_t mip, const void *data, size_t size) {
    const auto &target = get(texture);
    const auto &desc = target.texture;
    const VkExtent3D extent{std::max(desc.width >> mip, 1u), std::max(desc.height >> mip, 1u),
                            desc.type == numos::TextureType::Tex3D ? std::max(desc.depth >> mip, 1u) : 1u};
    const size_t expected = size_t(extent.width) * extent.height * extent.depth * layers(desc) * texelBytes(desc.format);
    if (size < expected)
      throw std::runtime_error("NUMOS texture upload is smaller than the mip");
    auto [staging, source] = stage(data, expected);
    const VkBufferImageCopy region{.bufferOffset = source,
                                   .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, layers(desc)},
                                   .imageExtent = extent};
    transferBarrier();
    vkCmdCopyBufferToImage(m_command, staging->buffer, target.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    transferBarrier();
  }

  VkDescriptorSet Bridge::allocateSet(VkDescriptorSetLayout layout) {
    auto &slot = m_slots[m_slot];
    for (;; ++slot.pool) {
      if (slot.pool == slot.pools.size()) {
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, setsPerPool * 2},
                                              {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, setsPerPool * 8},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, setsPerPool * 4},
                                              {VK_DESCRIPTOR_TYPE_SAMPLER, setsPerPool * 2},
                                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setsPerPool},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, setsPerPool * 2},
                                              {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, setsPerPool}};
        const VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = setsPerPool,
                                              .poolSizeCount = uint32_t(std::size(sizes)),
                                              .pPoolSizes = sizes};
        VkDescriptorPool pool = VK_NULL_HANDLE;
        check(vkCreateDescriptorPool(m_device, &info, nullptr, &pool));
        slot.pools.push_back(pool);
      }
      const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = slot.pools[slot.pool],
                                                 .descriptorSetCount = 1,
                                                 .pSetLayouts = &layout};
      VkDescriptorSet set = VK_NULL_HANDLE;
      const auto result = vkAllocateDescriptorSets(m_device, &allocate, &set);
      if (result == VK_SUCCESS)
        return set;
      if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
        throw result;
    }
  }

  numos::GpuHandle Bridge::placeholder(const numos::PassBindingInfo &binding) {
    // Key: kind, dimension and storage format.
    const uint32_t key = uint32_t(binding.kind) | uint32_t(binding.dim) << 8 | binding.spirvImageFormat << 16;
    if (auto found = m_placeholders.find(key); found != m_placeholders.end())
      return found->second;
    numos::GpuHandle handle = 0;
    switch (binding.kind) {
    case numos::BindingKind::Sampler:
      handle = createSampler({});
      break;
    case numos::BindingKind::ConstantBuffer:
      handle = createBuffer({.usage = numos::BufferUsage::Constant, .size = 256});
      break;
    case numos::BindingKind::StructuredBuffer:
    case numos::BindingKind::RWStructuredBuffer:
      handle = createBuffer({.usage = numos::BufferUsage::Structured, .size = 256});
      break;
    default: {
      numos::TextureDesc desc;
      desc.type = binding.dim == numos::BindingDim::Tex3D        ? numos::TextureType::Tex3D
                  : binding.dim == numos::BindingDim::Tex2DArray ? numos::TextureType::Tex2DArray
                                                                 : numos::TextureType::Tex2D;
      desc.format = binding.kind == numos::BindingKind::StorageTexture ? fromSpirv(binding.spirvImageFormat)
                                                                        : numos::Format::RGBA16Float;
      desc.debugName = "NUMOS placeholder";
      handle = createTexture(desc);
      const float zero[4] = {};
      clearTexture(handle, zero);
    }
    }
    if (binding.kind == numos::BindingKind::ConstantBuffer || binding.kind == numos::BindingKind::StructuredBuffer ||
        binding.kind == numos::BindingKind::RWStructuredBuffer) {
      transferBarrier();
      vkCmdFillBuffer(m_command, get(handle).handle, 0, VK_WHOLE_SIZE, 0);
      transferBarrier();
    }
    m_placeholders.emplace(key, handle);
    return handle;
  }

  void Bridge::dispatch(const numos::DispatchDesc &desc) {
    if (desc.pass >= m_passes.size())
      throw std::runtime_error("NUMOS dispatched an unknown pass");
    auto &pass = m_passes[desc.pass];
    if (!pass.pipeline)
      throw std::runtime_error(std::string("NUMOS pass has no pipeline: ") + pass.info.name);
    if (desc.bindingCount != pass.info.bindingCount)
      throw std::runtime_error(std::string("NUMOS dispatch binding count differs: ") + pass.info.name);
    const auto set = allocateSet(pass.setLayout);
    std::vector<VkWriteDescriptorSet> writes(desc.bindingCount);
    std::vector<VkDescriptorImageInfo> images(desc.bindingCount);
    std::vector<VkDescriptorBufferInfo> buffers(desc.bindingCount);
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accels(desc.bindingCount);
    std::vector<VkAccelerationStructureKHR> accelHandles(desc.bindingCount);
    for (uint32_t i = 0; i < desc.bindingCount; ++i) {
      const auto &declared = pass.info.bindings[i];
      const auto &binding = desc.bindings[i];
      auto &write = writes[i];
      write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
               .dstSet = set,
               .dstBinding = declared.slot,
               .descriptorCount = 1,
               .descriptorType = descriptorType(declared.kind)};
      const auto handle = binding.resource ? binding.resource : placeholder(declared);
      switch (declared.kind) {
      case numos::BindingKind::ConstantBuffer:
      case numos::BindingKind::StructuredBuffer:
      case numos::BindingKind::RWStructuredBuffer: {
        const auto &buffer = get(handle);
        buffers[i] = {buffer.handle, binding.resource ? binding.offset : 0,
                      !binding.resource || binding.size == ~size_t(0) ? VK_WHOLE_SIZE : binding.size};
        write.pBufferInfo = &buffers[i];
        break;
      }
      case numos::BindingKind::Sampler:
        images[i] = {.sampler = get(handle).sampler};
        write.pImageInfo = &images[i];
        break;
      case numos::BindingKind::SampledTexture:
      case numos::BindingKind::StorageTexture:
      case numos::BindingKind::CombinedTextureSampler: {
        auto &texture = get(handle);
        const bool storage = declared.kind == numos::BindingKind::StorageTexture;
        images[i] = {.imageView = view(texture, binding.resource ? binding.mipLevel : 0,
                                       binding.resource ? binding.mipCount : ~0u, storage),
                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
        if (declared.kind == numos::BindingKind::CombinedTextureSampler)
          images[i].sampler = get(binding.sampler ? binding.sampler : placeholder({.kind = numos::BindingKind::Sampler})).sampler;
        write.pImageInfo = &images[i];
        break;
      }
      case numos::BindingKind::AccelerationStructure:
        accelHandles[i] = get(handle).accel;
        accels[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                     .accelerationStructureCount = 1,
                     .pAccelerationStructures = &accelHandles[i]};
        write.pNext = &accels[i];
        break;
      }
    }
    vkUpdateDescriptorSets(m_device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindPipeline(m_command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline);
    vkCmdBindDescriptorSets(m_command, VK_PIPELINE_BIND_POINT_COMPUTE, pass.layout, 0, 1, &set, 0, nullptr);
    if (pass.info.pushConstantSize) {
      std::vector<uint8_t> push(pass.info.pushConstantSize);
      if (desc.pushConstants)
        std::memcpy(push.data(), desc.pushConstants, std::min(desc.pushConstantSize, pass.info.pushConstantSize));
      vkCmdPushConstants(m_command, pass.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, uint32_t(push.size()), push.data());
    }
    vkCmdDispatch(m_command, desc.groupsX, desc.groupsY, desc.groupsZ);
  }

  void Bridge::barrier() { memoryBarrier(m_command, workStages, workAccess, workStages, workAccess); }

  bool Bridge::readBuffer(numos::GpuHandle readback, void *dst, size_t size) {
    const auto &buffer = get(readback);
    if (!buffer.mapped || !buffer.copyFrame || m_frame < buffer.copyFrame + gpuFrameCount)
      return false;
    std::memcpy(dst, buffer.mapped, std::min(size, buffer.buffer.size));
    return true;
  }

  void Bridge::beginZone(const char *name) {
    if (m_command)
      m_zones.push_back(std::make_unique<gpu::GpuScope>(m_command, name));
  }

  void Bridge::endZone() {
    if (!m_zones.empty())
      m_zones.pop_back();
  }
} // namespace remix::sky
