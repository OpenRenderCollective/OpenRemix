#include "metal/sky_bridge.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace remix::sky {
  using namespace metal;

  namespace {
    constexpr size_t constantRingBytes = 4u << 20;
    constexpr size_t stagingChunkBytes = 8u << 20;
    constexpr size_t argumentChunkBytes = 1u << 20;
    // Constant buffer and argument buffer offsets.
    constexpr size_t bufferAlignment = 256;

    MTLPixelFormat format(numos::Format value) {
      switch (value) {
      case numos::Format::R8Unorm:
        return MTLPixelFormatR8Unorm;
      case numos::Format::RGBA8Unorm:
        return MTLPixelFormatRGBA8Unorm;
      case numos::Format::R16Float:
        return MTLPixelFormatR16Float;
      case numos::Format::RGBA16Float:
        return MTLPixelFormatRGBA16Float;
      case numos::Format::R32Uint:
        return MTLPixelFormatR32Uint;
      case numos::Format::R32Float:
        return MTLPixelFormatR32Float;
      case numos::Format::RG32Float:
        return MTLPixelFormatRG32Float;
      case numos::Format::RGBA32Float:
        return MTLPixelFormatRGBA32Float;
      case numos::Format::RG16Float:
        return MTLPixelFormatRG16Float;
      }
      return MTLPixelFormatInvalid;
    }

    size_t texelBytes(numos::Format value) {
      switch (value) {
      case numos::Format::R8Unorm:
        return 1;
      case numos::Format::R16Float:
        return 2;
      case numos::Format::RGBA8Unorm:
      case numos::Format::R32Uint:
      case numos::Format::R32Float:
      case numos::Format::RG16Float:
        return 4;
      case numos::Format::RGBA16Float:
      case numos::Format::RG32Float:
        return 8;
      case numos::Format::RGBA32Float:
        return 16;
      }
      return 4;
    }

    // One texel of the format holding rgba (its first channels).
    std::vector<uint8_t> texel(numos::Format value, const float rgba[4]) {
      std::vector<uint8_t> bytes(texelBytes(value));
      const auto put = [&](auto *channels, int count) {
        for (int i = 0; i < count; ++i)
          channels[i] = decltype(+channels[0])(rgba[i]);
      };
      switch (value) {
      case numos::Format::R8Unorm:
      case numos::Format::RGBA8Unorm:
        for (size_t i = 0; i < bytes.size(); ++i)
          bytes[i] = uint8_t(std::clamp(rgba[i], 0.f, 1.f) * 255 + .5f);
        break;
      case numos::Format::R16Float:
      case numos::Format::RG16Float:
      case numos::Format::RGBA16Float:
        put(reinterpret_cast<__fp16 *>(bytes.data()), int(bytes.size() / 2));
        break;
      case numos::Format::R32Uint:
        put(reinterpret_cast<uint32_t *>(bytes.data()), 1);
        break;
      case numos::Format::R32Float:
      case numos::Format::RG32Float:
      case numos::Format::RGBA32Float:
        put(reinterpret_cast<float *>(bytes.data()), int(bytes.size() / 4));
        break;
      }
      return bytes;
    }

    // The numos::Format of a storage image's SPIR-V ImageFormat (placeholders).
    numos::Format fromSpirv(uint32_t value) {
      switch (value) {
      case 1:
        return numos::Format::RGBA32Float; // Rgba32f
      case 2:
        return numos::Format::RGBA16Float; // Rgba16f
      case 3:
        return numos::Format::R32Float; // R32f
      case 4:
        return numos::Format::RGBA8Unorm; // Rgba8
      case 6:
        return numos::Format::RG32Float; // Rg32f
      case 7:
        return numos::Format::RG16Float; // Rg16f
      case 9:
        return numos::Format::R16Float; // R16f
      case 15:
        return numos::Format::R8Unorm; // R8
      case 33:
        return numos::Format::R32Uint; // R32ui
      }
      return numos::Format::RGBA16Float;
    }

    MTLTextureType textureType(numos::TextureType type) {
      return type == numos::TextureType::Tex3D        ? MTLTextureType3D
             : type == numos::TextureType::Tex2DArray ? MTLTextureType2DArray
                                                      : MTLTextureType2D;
    }

    uint32_t layers(const numos::TextureDesc &desc) {
      return desc.type == numos::TextureType::Tex2DArray ? desc.depth : 1;
    }

    MTLSize mipSize(const numos::TextureDesc &desc, uint32_t mip) {
      return MTLSizeMake(std::max(desc.width >> mip, 1u), std::max(desc.height >> mip, 1u),
                         desc.type == numos::TextureType::Tex3D ? std::max(desc.depth >> mip, 1u) : 1u);
    }

    size_t align(size_t value, size_t alignment) { return (value + alignment - 1) / alignment * alignment; }
  } // namespace

  Bridge::Bridge(PhysicalDevice, Device device, std::vector<HostPass> hostPasses) : m_device(device) {
    std::vector<numos::PassInfo> passes;
    for (uint32_t i = 0; i < numos::passCount(); ++i)
      passes.push_back(numos::passInfo(i));
    for (const auto &host : hostPasses)
      passes.push_back(host.info);
    for (uint32_t i = 0; i < passes.size(); ++i) {
      Pass pass{.info = passes[i], .stage = metal::shaders::Stage(uint32_t(metal::shaders::Stage::sky0) + i)};
      for (uint32_t b = 0; b < pass.info.bindingCount; ++b)
        if (pass.info.bindings[b].set != 0)
          throw Error("NUMOS pass binds a descriptor set other than 0");
      pass.set = ArgumentSet(device, pass.stage, 0);
      m_passes.push_back(std::move(pass));
    }
    for (auto &slot : m_slots) {
      Resource ring{.kind = Resource::Kind::Buffer, .imported = true};
      ring.buffer = {.usage = numos::BufferUsage::Constant, .size = constantRingBytes};
      ring.handle = metal::buffer(device, nullptr, constantRingBytes);
      slot.constants = add(std::move(ring));
    }
  }

  Bridge::~Bridge() = default;

  numos::GpuHandle Bridge::add(Resource resource) {
    const auto handle = m_next++;
    m_resources.emplace(handle, std::move(resource));
    return handle;
  }

  Bridge::Resource &Bridge::get(numos::GpuHandle handle) {
    auto found = m_resources.find(handle);
    if (found == m_resources.end())
      throw Error("Unknown NUMOS GPU handle");
    return found->second;
  }

  const Bridge::Resource &Bridge::get(numos::GpuHandle handle) const {
    auto found = m_resources.find(handle);
    if (found == m_resources.end())
      throw Error("Unknown NUMOS GPU handle");
    return found->second;
  }

  id<MTLCommandBuffer> Bridge::command() const {
    if (!m_command)
      throw Error("NUMOS recorded GPU work outside a frame");
    return m_command;
  }

  void Bridge::selectFrame(uint32_t slot) {
    m_slot = slot % gpuFrameCount;
    ++m_frame;
    auto &s = m_slots[m_slot];
    // Metal releases an object when the last command buffer using it completes.
    s.retired.clear();
    s.constantsUsed = 0;
    for (auto &ring : s.staging)
      ring.used = 0;
    for (auto &ring : s.arguments)
      ring.used = 0;
  }

  bool Bridge::preparePipelines(const std::function<bool(uint32_t pass)> &needed, uint32_t &ready, uint32_t &total) {
    ready = total = 0;
    for (uint32_t index = 0; index < m_passes.size(); ++index) {
      auto &pass = m_passes[index];
      if (!needed(index))
        continue;
      ++total;
      if (!pass.pipeline)
        pass.pipeline = computePipelineAsync(m_device, pass.stage, 0);
      ready += pass.pipeline != nil;
    }
    return ready == total;
  }

  void Bridge::begin(id<MTLCommandBuffer> command) { m_command = command; }

  void Bridge::end() {
    m_zones.clear();
    m_command = nil;
  }

  id<MTLTexture> Bridge::sampledView(numos::GpuHandle texture) { return view(get(texture), 0, ~0u, false); }

  id<MTLSamplerState> Bridge::sampler(numos::GpuHandle sampler) const { return get(sampler).sampler; }

  numos::GpuHandle Bridge::importTexture(Image, ImageView view, const numos::TextureDesc &desc) {
    Resource resource{.kind = Resource::Kind::Texture, .imported = true, .texture = desc, .image = view};
    return add(std::move(resource));
  }

  numos::GpuHandle Bridge::importAccelerationStructure(AccelerationStructure scene) {
    Resource resource{.kind = Resource::Kind::Accel, .imported = true};
    resource.scene = scene;
    return add(std::move(resource));
  }

  void Bridge::forget(numos::GpuHandle handle) {
    auto found = m_resources.find(handle);
    if (found != m_resources.end() && found->second.imported)
      m_resources.erase(found);
  }

  id<MTLTexture> Bridge::view(Resource &texture, uint32_t mip, uint32_t count, bool storage) {
    const uint32_t levels = uint32_t(texture.image.mipmapLevelCount);
    mip = std::min(mip, levels - 1);
    count = storage ? 1 : std::min(count, levels - mip);
    if (mip == 0 && count == levels)
      return texture.image;
    const uint64_t key = uint64_t(mip) | uint64_t(count) << 16;
    if (auto found = texture.views.find(key); found != texture.views.end())
      return found->second;
    id<MTLTexture> view = [texture.image newTextureViewWithPixelFormat:texture.image.pixelFormat
                                                           textureType:texture.image.textureType
                                                                levels:NSMakeRange(mip, count)
                                                                slices:NSMakeRange(0, texture.image.arrayLength)];
    check(view, "NUMOS texture view");
    texture.views.emplace(key, view);
    return view;
  }

  numos::GpuHandle Bridge::createTexture(const numos::TextureDesc &desc) {
    auto *descriptor = [MTLTextureDescriptor new];
    descriptor.textureType = textureType(desc.type);
    descriptor.pixelFormat = format(desc.format);
    descriptor.width = std::max(desc.width, 1u);
    descriptor.height = std::max(desc.height, 1u);
    descriptor.depth = desc.type == numos::TextureType::Tex3D ? std::max(desc.depth, 1u) : 1u;
    descriptor.arrayLength = layers(desc);
    descriptor.mipmapLevelCount = std::max(desc.mipLevels, 1u);
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    descriptor.storageMode = MTLStorageModePrivate;
    Resource resource{.kind = Resource::Kind::Texture, .texture = desc};
    resource.image = [m_device newTextureWithDescriptor:descriptor];
    check(resource.image, "NUMOS texture allocation");
    if (desc.debugName)
      resource.image.label = @(desc.debugName);
    return add(std::move(resource));
  }

  numos::GpuHandle Bridge::createBuffer(const numos::BufferDesc &desc) {
    Resource resource{.kind = Resource::Kind::Buffer, .buffer = desc};
    // Readback buffers are read by the host; the others change only through
    // recorded copies, ordered with the dispatches.
    resource.handle =
        [m_device newBufferWithLength:std::max<size_t>(desc.size, 16)
                              options:desc.usage == numos::BufferUsage::Readback ? MTLResourceStorageModeShared
                                                                                 : MTLResourceStorageModePrivate];
    check(resource.handle, "NUMOS buffer allocation");
    if (desc.debugName)
      resource.handle.label = @(desc.debugName);
    return add(std::move(resource));
  }

  numos::GpuHandle Bridge::createSampler(const numos::SamplerDesc &desc) {
    const auto address = [](numos::AddressMode mode) {
      return mode == numos::AddressMode::Repeat ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    };
    auto *descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = descriptor.magFilter =
        desc.filter == numos::Filter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter =
        desc.mipFilter == numos::Filter::Linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    descriptor.sAddressMode = address(desc.addressU);
    descriptor.tAddressMode = address(desc.addressV);
    descriptor.rAddressMode = address(desc.addressW);
    descriptor.lodMaxClamp = desc.maxLod >= numos::SamplerDesc::kLodClampNone ? FLT_MAX : desc.maxLod;
    descriptor.supportArgumentBuffers = YES;
    Resource resource{.kind = Resource::Kind::Sampler};
    resource.sampler = [m_device newSamplerStateWithDescriptor:descriptor];
    check(resource.sampler, "NUMOS sampler");
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
    const size_t offset = align(slot.constantsUsed, bufferAlignment);
    if (offset + size > constantRingBytes)
      throw Error("NUMOS constants exceed the frame's ring");
    std::memcpy(static_cast<uint8_t *>(ring.handle.contents) + offset, data, size);
    slot.constantsUsed = offset + size;
    return {slot.constants, offset, size};
  }

  std::pair<id<MTLBuffer>, size_t> Bridge::allocate(std::vector<Ring> &rings, size_t size, size_t alignment,
                                                    size_t chunk) {
    for (auto &ring : rings)
      if (align(ring.used, alignment) + size <= ring.buffer.length) {
        const size_t offset = align(ring.used, alignment);
        ring.used = offset + size;
        return {ring.buffer, offset};
      }
    rings.push_back({metal::buffer(m_device, nullptr, std::max(size, chunk)), size});
    return {rings.back().buffer, 0};
  }

  std::pair<id<MTLBuffer>, size_t> Bridge::stage(const void *data, size_t size) {
    auto [buffer, offset] = allocate(m_slots[m_slot].staging, size, 16, stagingChunkBytes);
    std::memcpy(static_cast<uint8_t *>(buffer.contents) + offset, data, size);
    return {buffer, offset};
  }

  void Bridge::updateBuffer(numos::GpuHandle buffer, size_t offset, size_t size, const void *data) {
    if (!size)
      return;
    auto [staging, source] = stage(data, size);
    id<MTLBlitCommandEncoder> blit = blitEncoder(command(), @"NUMOS buffer update");
    [blit copyFromBuffer:staging sourceOffset:source toBuffer:get(buffer).handle destinationOffset:offset size:size];
    [blit endEncoding];
  }

  void Bridge::copyBuffer(numos::GpuHandle dst, size_t dstOffset, numos::GpuHandle src, size_t srcOffset, size_t size) {
    auto &target = get(dst);
    id<MTLBlitCommandEncoder> blit = blitEncoder(command(), @"NUMOS buffer copy");
    [blit copyFromBuffer:get(src).handle
             sourceOffset:srcOffset
                 toBuffer:target.handle
        destinationOffset:dstOffset
                     size:size];
    [blit endEncoding];
    if (target.buffer.usage == numos::BufferUsage::Readback)
      target.copyFrame = m_frame;
  }

  void Bridge::copyTexture(numos::GpuHandle dst, numos::GpuHandle src) {
    id<MTLBlitCommandEncoder> blit = blitEncoder(command(), @"NUMOS texture copy");
    [blit copyFromTexture:get(src).image toTexture:get(dst).image];
    [blit endEncoding];
  }

  void Bridge::copyToTexture(id<MTLBuffer> source, size_t offset, size_t rowBytes, Resource &target, uint32_t mip) {
    const auto &desc = target.texture;
    const MTLSize size = mipSize(desc, mip);
    const size_t imageBytes = rowBytes * size.height;
    id<MTLBlitCommandEncoder> blit = blitEncoder(command(), @"NUMOS texture upload");
    for (uint32_t layer = 0; layer < layers(desc); ++layer)
      [blit copyFromBuffer:source
                 sourceOffset:offset + layer * imageBytes * size.depth
            sourceBytesPerRow:rowBytes
          sourceBytesPerImage:imageBytes
                   sourceSize:size
                    toTexture:target.image
             destinationSlice:layer
             destinationLevel:mip
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
  }

  void Bridge::clearTexture(numos::GpuHandle texture, const float rgba[4]) {
    auto &target = get(texture);
    const auto &desc = target.texture;
    const auto value = texel(desc.format, rgba);
    // The largest mip's texels for every layer, filled with the value.
    const MTLSize base = mipSize(desc, 0);
    const size_t texels = base.width * base.height * base.depth * layers(desc);
    auto [buffer, offset] = allocate(m_slots[m_slot].staging, texels * value.size(), 16, stagingChunkBytes);
    auto *bytes = static_cast<uint8_t *>(buffer.contents) + offset;
    if (std::all_of(value.begin(), value.end(), [](uint8_t b) { return b == 0; }))
      std::memset(bytes, 0, texels * value.size());
    else
      for (size_t i = 0; i < texels; ++i)
        std::memcpy(bytes + i * value.size(), value.data(), value.size());
    const uint32_t levels = uint32_t(target.image.mipmapLevelCount);
    for (uint32_t mip = 0; mip < levels; ++mip)
      copyToTexture(buffer, offset, mipSize(desc, mip).width * value.size(), target, mip);
  }

  void Bridge::uploadTexture(numos::GpuHandle texture, uint32_t mip, const void *data, size_t size) {
    auto &target = get(texture);
    const auto &desc = target.texture;
    const MTLSize extent = mipSize(desc, mip);
    const size_t rowBytes = extent.width * texelBytes(desc.format);
    const size_t expected = rowBytes * extent.height * extent.depth * layers(desc);
    if (size < expected)
      throw Error("NUMOS texture upload is smaller than the mip");
    auto [staging, source] = stage(data, expected);
    copyToTexture(staging, source, rowBytes, target, mip);
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
    case numos::BindingKind::StructuredBuffer:
    case numos::BindingKind::RWStructuredBuffer: {
      handle = createBuffer({.usage = numos::BufferUsage::Structured, .size = 256});
      id<MTLBlitCommandEncoder> blit = blitEncoder(command(), @"NUMOS placeholder");
      [blit fillBuffer:get(handle).handle range:NSMakeRange(0, 256) value:0];
      [blit endEncoding];
      break;
    }
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
    m_placeholders.emplace(key, handle);
    return handle;
  }

  void Bridge::dispatch(const numos::DispatchDesc &desc) {
    if (desc.pass >= m_passes.size())
      throw Error("NUMOS dispatched an unknown pass");
    auto &pass = m_passes[desc.pass];
    if (!pass.pipeline)
      throw Error(std::string("NUMOS pass has no pipeline: ") + pass.info.name);
    if (desc.bindingCount != pass.info.bindingCount)
      throw Error(std::string("NUMOS dispatch binding count differs: ") + pass.info.name);
    // Placeholders first: they may record clears, which end before the dispatch's encoder.
    std::vector<numos::GpuHandle> handles(desc.bindingCount);
    for (uint32_t i = 0; i < desc.bindingCount; ++i)
      handles[i] = desc.bindings[i].resource ? desc.bindings[i].resource : placeholder(pass.info.bindings[i]);
    auto [arguments, argumentOffset] =
        allocate(m_slots[m_slot].arguments, pass.set.encodedLength(), bufferAlignment, argumentChunkBytes);
    auto &set = pass.set;
    set.begin(arguments, argumentOffset);
    std::vector<id<MTLResource>> referenced;
    for (uint32_t i = 0; i < desc.bindingCount; ++i) {
      const auto &declared = pass.info.bindings[i];
      const auto &binding = desc.bindings[i];
      auto &resource = get(handles[i]);
      switch (declared.kind) {
      case numos::BindingKind::ConstantBuffer:
      case numos::BindingKind::StructuredBuffer:
      case numos::BindingKind::RWStructuredBuffer:
        set.buffer(declared.slot, resource.handle, binding.resource ? binding.offset : 0);
        break;
      case numos::BindingKind::Sampler:
        set.sampler(declared.slot, resource.sampler);
        break;
      case numos::BindingKind::SampledTexture:
      case numos::BindingKind::StorageTexture:
      case numos::BindingKind::CombinedTextureSampler: {
        const bool storage = declared.kind == numos::BindingKind::StorageTexture;
        id<MTLTexture> texture =
            view(resource, binding.resource ? binding.mipLevel : 0, binding.resource ? binding.mipCount : ~0u, storage);
        id<MTLSamplerState> sampler = nil;
        if (declared.kind == numos::BindingKind::CombinedTextureSampler)
          sampler = get(binding.sampler ? binding.sampler : placeholder({.kind = numos::BindingKind::Sampler})).sampler;
        set.texture(declared.slot, texture, sampler);
        break;
      }
      case numos::BindingKind::AccelerationStructure:
        set.acceleration(declared.slot, resource.scene->tlas);
        referenced.insert(referenced.end(), resource.scene->referenced.begin(), resource.scene->referenced.end());
        break;
      }
    }
    id<MTLComputeCommandEncoder> encoder =
        computeEncoder(command(), [NSString stringWithFormat:@"NUMOS %s", pass.info.name]);
    [encoder setComputePipelineState:pass.pipeline];
    [encoder setBuffer:arguments offset:argumentOffset atIndex:0];
    set.use(encoder);
    if (!referenced.empty())
      [encoder useResources:referenced.data() count:referenced.size() usage:MTLResourceUsageRead];
    const auto &source = library(pass.stage);
    if (pass.info.pushConstantSize && source.push != 0xff) {
      std::vector<uint8_t> push(pass.info.pushConstantSize);
      if (desc.pushConstants)
        std::memcpy(push.data(), desc.pushConstants, std::min(desc.pushConstantSize, pass.info.pushConstantSize));
      [encoder setBytes:push.data() length:push.size() atIndex:source.push];
    }
    [encoder dispatchThreadgroups:MTLSizeMake(desc.groupsX, desc.groupsY, desc.groupsZ)
            threadsPerThreadgroup:MTLSizeMake(source.threads[0], source.threads[1], source.threads[2])];
    [encoder endEncoding];
  }

  bool Bridge::readBuffer(numos::GpuHandle readback, void *dst, size_t size) {
    const auto &buffer = get(readback);
    if (buffer.buffer.usage != numos::BufferUsage::Readback || !buffer.copyFrame ||
        m_frame < buffer.copyFrame + gpuFrameCount)
      return false;
    std::memcpy(dst, buffer.handle.contents, std::min(size, buffer.buffer.size));
    return true;
  }

  void Bridge::beginZone(const char *name) { m_zones.emplace_back(name ? name : ""); }

  void Bridge::endZone() {
    if (!m_zones.empty())
      m_zones.pop_back();
  }
} // namespace remix::sky
