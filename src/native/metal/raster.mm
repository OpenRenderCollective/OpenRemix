#include "metal/raster_gpu.h"
#include "platform/timing.h"
#include "scene/images.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <remix/remix_native_ext.h>

namespace remix::metal {
  using namespace scene;

  namespace {
    float linear(float c) { return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f); }
    // Vulkan enumerants as stored in scene::Blend and scene::Draw.
    MTLBlendFactor blendFactor(uint32_t factor) {
      static constexpr MTLBlendFactor table[] = {
          MTLBlendFactorZero,
          MTLBlendFactorOne,
          MTLBlendFactorSourceColor,
          MTLBlendFactorOneMinusSourceColor,
          MTLBlendFactorDestinationColor,
          MTLBlendFactorOneMinusDestinationColor,
          MTLBlendFactorSourceAlpha,
          MTLBlendFactorOneMinusSourceAlpha,
          MTLBlendFactorDestinationAlpha,
          MTLBlendFactorOneMinusDestinationAlpha,
          MTLBlendFactorBlendColor,
          MTLBlendFactorOneMinusBlendColor,
          MTLBlendFactorBlendAlpha,
          MTLBlendFactorOneMinusBlendAlpha,
          MTLBlendFactorSourceAlphaSaturated,
          MTLBlendFactorSource1Color,
          MTLBlendFactorOneMinusSource1Color,
          MTLBlendFactorSource1Alpha,
          MTLBlendFactorOneMinusSource1Alpha,
      };
      return factor < std::size(table) ? table[factor] : MTLBlendFactorOne;
    }
    // VkBlendOp and VkCompareOp share Metal's numbering.
    MTLBlendOperation blendOperation(uint32_t op) { return op <= 4 ? MTLBlendOperation(op) : MTLBlendOperationAdd; }
    MTLColorWriteMask writeMask(uint32_t mask) {
      return (mask & 1 ? MTLColorWriteMaskRed : 0) | (mask & 2 ? MTLColorWriteMaskGreen : 0) |
             (mask & 4 ? MTLColorWriteMaskBlue : 0) | (mask & 8 ? MTLColorWriteMaskAlpha : 0);
    }
    struct BlockFormat {
      MTLPixelFormat format = MTLPixelFormatInvalid;
      bool opaqueAlpha = false; // BC1 RGB: alpha samples as one
    };
    BlockFormat blockFormat(uint32_t format) {
      switch (format) {
      case REMIXAPI_FORMAT_BC1_RGB_SRGB:
        return {MTLPixelFormatBC1_RGBA_sRGB, true};
      case REMIXAPI_FORMAT_BC1_RGB_UNORM:
        return {MTLPixelFormatBC1_RGBA, true};
      case REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT:
        return {MTLPixelFormatBC1_RGBA_sRGB};
      case REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT:
        return {MTLPixelFormatBC1_RGBA};
      case REMIXAPI_FORMAT_BC3_SRGB:
        return {MTLPixelFormatBC3_RGBA_sRGB};
      case REMIXAPI_FORMAT_BC3_UNORM:
        return {MTLPixelFormatBC3_RGBA};
      case REMIXAPI_FORMAT_BC4_UNORM_EXT:
        return {MTLPixelFormatBC4_RUnorm};
      case REMIXAPI_FORMAT_BC5_UNORM:
        return {MTLPixelFormatBC5_RGUnorm};
      case REMIXAPI_FORMAT_BC6H_UFLOAT_EXT:
        return {MTLPixelFormatBC6H_RGBUfloat};
      case REMIXAPI_FORMAT_BC6H_SFLOAT_EXT:
        return {MTLPixelFormatBC6H_RGBFloat};
      case REMIXAPI_FORMAT_BC7_SRGB:
        return {MTLPixelFormatBC7_RGBAUnorm_sRGB};
      case REMIXAPI_FORMAT_BC7_UNORM:
        return {MTLPixelFormatBC7_RGBAUnorm};
      default:
        return {};
      }
    }
    // Same-class UNORM counterpart used by linear data-map views.
    MTLPixelFormat linearFormat(MTLPixelFormat format) {
      switch (format) {
      case MTLPixelFormatRGBA8Unorm_sRGB:
        return MTLPixelFormatRGBA8Unorm;
      case MTLPixelFormatBC1_RGBA_sRGB:
        return MTLPixelFormatBC1_RGBA;
      case MTLPixelFormatBC3_RGBA_sRGB:
        return MTLPixelFormatBC3_RGBA;
      case MTLPixelFormatBC7_RGBAUnorm_sRGB:
        return MTLPixelFormatBC7_RGBAUnorm;
      default:
        return format;
      }
    }
    // raster/pipelines.cpp pipelineKey, plus the target and the stages.
    constexpr uint64_t skyBit = uint64_t(1) << 34, maskedBit = uint64_t(1) << 35, spriteBit = uint64_t(1) << 36;
    // remixapi_InstanceInfoWireframeEXT (pipelines.cpp's bit 34 is the target
    // here): lines are encoder state, not part of the pipeline.
    constexpr uint64_t wireframeBit = uint64_t(1) << 37;
  } // namespace

  Raster::Raster() = default;
  Raster::~Raster() = default;

  void Raster::initializeGpu(void *device) {
    m_gpu = std::make_unique<Gpu>();
    m_gpu->device = (__bridge id<MTLDevice>)device;
  }

  void Raster::shutdownGpu() { m_gpu.reset(); }

  void Raster::selectFrame(uint32_t slot) {
    auto &g = *m_gpu;
    g.frameIndex = slot % gpuFrameCount;
    g.frames[g.frameIndex].retired.clear();
  }

  Raster::MemoryStatistics Raster::memoryStatistics() const {
    MemoryStatistics result;
    if (!m_gpu)
      return result;
    for (const auto &[_, texture] : m_gpu->textures) {
      if (!texture.bytes)
        continue;
      result.textureBytes += texture.bytes;
      ++result.textureCount;
      if (texture.compressed)
        result.compressedTextureBytes += texture.bytes;
    }
    for (const auto &[_, mesh] : m_gpu->meshes)
      result.geometryBytes += mesh.data.allocatedSize;
    return result;
  }

  void Raster::Gpu::configureTarget(MTLPixelFormat format, uint32_t w, uint32_t h, bool encode) {
    if (format != displayFormat || encode != encodeOutput) {
      pipelines.clear();
      halo.composite[0] = halo.composite[1] = nil;
      displayFormat = format;
      encodeOutput = encode;
      // UNORM targets blend display-encoded values (halo composite flags bit 1).
      linearTarget = format != MTLPixelFormatBGRA8Unorm && format != MTLPixelFormatRGBA8Unorm &&
                     format != MTLPixelFormatBGR10A2Unorm && format != MTLPixelFormatRGB10A2Unorm;
    }
    if (!depth || width != w || height != h) {
      retire(depth);
      depth = texture2d(device, depthFormat, w, h, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
      width = w;
      height = h;
      // Coverage targets and the sprite depth copy are at target resolution.
      for (auto &target : halo.targets) {
        retire(target.texture);
        target = {};
      }
      for (auto &target : mask.targets)
        retire(target.texture);
      mask.targets.clear();
      retire(sprite.depth);
      sprite.depth = nil;
      sprite.set = {};
    }
  }

  Raster::Gpu::MeshGpu &Raster::Gpu::mesh(const std::shared_ptr<Surface> &source) {
    if (auto it = meshes.find(source.get()); it != meshes.end())
      return it->second;
    const size_t offset = source->vertices.size() * sizeof(Vertex);
    const size_t indexBytes = source->indices.size() * sizeof(uint32_t);
    // Sprite vertices (remixapi_MeshInfoSpriteVerticesEXT) follow the indices.
    const size_t spriteOffset = offset + indexBytes;
    const size_t spriteBytes = source->sprite.size() * sizeof(SpriteVertex);
    MeshGpu value{source, buffer(device, nullptr, spriteOffset + spriteBytes), offset, spriteOffset};
    auto *bytes = static_cast<uint8_t *>(value.data.contents);
    std::memcpy(bytes, source->vertices.data(), offset);
    std::memcpy(bytes + offset, source->indices.data(), indexBytes);
    if (spriteBytes)
      std::memcpy(bytes + spriteOffset, source->sprite.data(), spriteBytes);
    uploadBytes += spriteOffset + spriteBytes;
    return meshes.emplace(source.get(), std::move(value)).first->second;
  }

  // raster/resources.cpp texture: updatable textures (remixapi_UpdateTextureEXT)
  // start from a consistent state at their current revision, then upload the
  // region each update dirtied.
  Raster::Gpu::TextureGpu &Raster::Gpu::texture(const std::shared_ptr<Texture> &source) {
    if (auto it = textures.find(source.get()); it != textures.end()) {
      if (source->updates)
        refreshTexture(it->second, *source);
      return it->second;
    }
    if (source->pixels.compressed)
      return compressedTexture(source);
    if (!source->updates)
      return textures.emplace(source.get(), uploadPixels(source, source->pixels)).first->second;
    auto &updates = *source->updates;
    std::lock_guard lock(updates.lock);
    auto &value = textures.emplace(source.get(), uploadPixels(source, source->pixels)).first->second;
    value.revision = updates.revision;
    std::fill_n(updates.dirty, 4, 0u);
    return value;
  }

  void Raster::Gpu::refreshTexture(TextureGpu &value, const Texture &source) {
    auto &updates = *source.updates;
    std::lock_guard lock(updates.lock);
    if (updates.revision == value.revision)
      return;
    value.revision = updates.revision;
    const uint32_t x0 = updates.dirty[0], y0 = updates.dirty[1], x1 = updates.dirty[2], y1 = updates.dirty[3];
    std::fill_n(updates.dirty, 4, 0u);
    const auto &p = source.pixels;
    // Region updates cover plain RGBA8 images at full size. One with mips
    // (generated again by every update, raster/resources.cpp refreshLevels)
    // is uploaded whole, every level.
    if (x1 <= x0 || y1 <= y0 || !value.texture || p.compressed || !p.hdrMips.empty() || p.mips.empty() ||
        value.texture.width != p.width || value.texture.height != p.height || x1 > p.width || y1 > p.height)
      return;
    dynamicTextureUploads += source.dynamic;
    if (p.mips.size() > 1) {
      const uint32_t levels = std::min<uint32_t>(uint32_t(p.mips.size()), uint32_t(value.texture.mipmapLevelCount));
      for (uint32_t level = 0; level < levels; ++level) {
        const uint32_t w = std::max(1u, p.width >> level), h = std::max(1u, p.height >> level);
        if (p.mips[level].size() >= size_t(w) * h * 4)
          [value.texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                           mipmapLevel:level
                             withBytes:p.mips[level].data()
                           bytesPerRow:size_t(w) * 4];
      }
      return;
    }
    // A shared texture is written in place. Unlike Vulkan's copy (ordered
    // after earlier frames' reads), a frame still in flight may sample the
    // new texels of this region.
    [value.texture replaceRegion:MTLRegionMake2D(x0, y0, x1 - x0, y1 - y0)
                     mipmapLevel:0
                       withBytes:p.mips[0].data() + (size_t(y0) * p.width + x0) * 4
                     bytesPerRow:size_t(p.width) * 4];
  }

  uint64_t Raster::dynamicTextureGeneration() const { return m_gpu ? m_gpu->dynamicTextureUploads : 0; }

  Raster::Gpu::TextureGpu Raster::Gpu::uploadPixels(const std::shared_ptr<Texture> &owner, const Pixels &p) {
    const uint32_t levels = std::max<uint32_t>(1, uint32_t(p.hdrMips.empty() ? p.mips.size() : p.hdrMips.size()));
    const auto format = !p.hdrMips.empty() ? MTLPixelFormatRGBA32Float
                        : p.srgb           ? MTLPixelFormatRGBA8Unorm_sRGB
                                           : MTLPixelFormatRGBA8Unorm;
    const size_t texel = !p.hdrMips.empty() ? 16 : 4;
    // sRGB images may later gain a linear alias view (data maps).
    auto usage = MTLTextureUsageShaderRead | (p.srgb ? MTLTextureUsagePixelFormatView : 0);
    // Unified memory: shared textures are written directly, as host-visible staging would be.
    TextureGpu value{owner, texture2d(device, format, p.width, p.height, usage, levels, MTLStorageModeShared)};
    uint32_t width = std::max(1u, p.width), height = std::max(1u, p.height);
    for (uint32_t i = 0; i < levels; ++i) {
      const void *bytes = p.hdrMips.empty() ? static_cast<const void *>(p.mips[i].data())
                                            : static_cast<const void *>(p.hdrMips[i].data());
      const size_t size = p.hdrMips.empty() ? p.mips[i].size() : p.hdrMips[i].size() * sizeof(float);
      if (size >= size_t(width) * height * texel)
        [value.texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                         mipmapLevel:i
                           withBytes:bytes
                         bytesPerRow:width * texel];
      value.bytes += size_t(width) * height * texel;
      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }
    uploadBytes += value.bytes;
    return value;
  }

  Raster::Gpu::TextureGpu &Raster::Gpu::compressedTexture(const std::shared_ptr<Texture> &source) {
    const auto &p = source->pixels;
    auto &c = *p.compressed;
    // A linear data map of an sRGB compressed texture shares the source's
    // image through a UNORM view: no second copy.
    if (const auto &base = source->encodedSource; base && base->pixels.compressed == p.compressed) {
      auto &image = texture(base);
      const auto format = linearFormat(image.texture.pixelFormat);
      TextureGpu value{source, format == image.texture.pixelFormat
                                   ? image.texture
                                   : [image.texture newTextureViewWithPixelFormat:format]};
      return textures.emplace(source.get(), std::move(value)).first->second;
    }
    auto release = [&] { std::vector<uint8_t>().swap(c.upload); };
    const auto block = device.supportsBCTextureCompression ? blockFormat(c.format) : BlockFormat{};
    if (c.upload.empty() || block.format == MTLPixelFormatInvalid) {
      // Without BC sampling, decode once from the payload (raster/resources.cpp).
      Pixels fallback;
      if (!c.upload.empty()) {
        fallback = decodeCompressed(p);
      } else {
        fallback = {c.previewWidth, c.previewHeight, p.srgb, {c.preview}};
        if (!c.hdrPreview.empty())
          fallback.hdrMips = {c.hdrPreview};
      }
      auto value = uploadPixels(source, fallback);
      release();
      return textures.emplace(source.get(), std::move(value)).first->second;
    }
    auto *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:block.format
                                                                         width:p.width
                                                                        height:p.height
                                                                     mipmapped:NO];
    descriptor.mipmapLevelCount = c.levels;
    descriptor.usage = MTLTextureUsageShaderRead | (p.srgb ? MTLTextureUsagePixelFormatView : 0);
    descriptor.storageMode = MTLStorageModeShared;
    // BC4 is sampled as (r, r, r, 1) and BC1 RGB with opaque alpha, as the Vulkan views.
    if (c.block == 4)
      descriptor.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed,
                                                         MTLTextureSwizzleRed, MTLTextureSwizzleOne);
    else if (block.opaqueAlpha)
      descriptor.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                                         MTLTextureSwizzleBlue, MTLTextureSwizzleOne);
    TextureGpu value{source, [device newTextureWithDescriptor:descriptor]};
    check(value.texture, "compressed texture");
    value.compressed = true;
    const size_t blockSize = (c.block == 1 || c.block == 4) ? 8 : 16;
    size_t offset = 0;
    uint32_t width = p.width, height = p.height;
    for (uint32_t i = 0; i < c.levels; ++i) {
      const size_t row = size_t((width + 3) / 4) * blockSize;
      const size_t size = row * ((height + 3) / 4);
      if (offset + size > c.upload.size())
        throw Error("compressed texture payload is truncated");
      [value.texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
                       mipmapLevel:i
                         withBytes:c.upload.data() + offset
                       bytesPerRow:row];
      offset += size;
      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }
    value.bytes = offset;
    uploadBytes += offset;
    // The image is now the only copy.
    release();
    return textures.emplace(source.get(), std::move(value)).first->second;
  }

  Raster::Gpu::MaterialGpu &Raster::Gpu::material(const std::shared_ptr<Material> &source) {
    if (auto it = materials.find(source.get()); it != materials.end())
      return *it->second;
    auto &tex = texture(source->texture);
    auto value = std::make_unique<MaterialGpu>();
    value->owner = source;
    value->sampler = metal::sampler(device, source->filter, source->wrapU, source->wrapV);
    value->set = ArgumentSet(device, shaders::Stage::rasterFrag, 0);
    value->set.begin(0);
    value->set.texture(0, tex.texture, value->sampler);
    return *materials.emplace(source.get(), std::move(value)).first->second;
  }

  SampledImage Raster::Gpu::sampledMaterial(const std::shared_ptr<Material> &source) {
    auto &mat = material(source);
    return {textures.at(source->texture.get()).texture, mat.sampler};
  }

  SampledImage Raster::Gpu::sampledTexture(const std::shared_ptr<Material> &source,
                                           const std::shared_ptr<Texture> &image) {
    auto &mat = material(source);
    return {texture(image).texture, mat.sampler};
  }

  uint64_t Raster::Gpu::key(const Draw &draw, Target target) {
    const auto &b = draw.blend;
    // SKY instances skip the depth test in the shared main pass (their camera
    // is not the world's). The sky target has its own depth, so there the 3D
    // skybox's geometry occludes itself.
    const bool depth = draw.depth || (target == Target::Sky && draw.camera == REMIXAPI_CAMERA_TYPE_SKY);
    return uint64_t(b.src) | uint64_t(b.dst) << 4 | uint64_t(b.op) << 8 | uint64_t(b.srcAlpha) << 11 |
           uint64_t(b.dstAlpha) << 15 | uint64_t(b.alphaOp) << 19 | uint64_t(b.mask) << 22 |
           uint64_t(b.enabled) << 26 | uint64_t(draw.doubleSided) << 27 | uint64_t(depth) << 28 |
           uint64_t(depth && (draw.depthWriteOverride ? draw.depthWrite : (!b.enabled || draw.ui))) << 29 |
           uint64_t(draw.depthCompare) << 30 | uint64_t(draw.depthBias) << 33 |
           (target == Target::Sky ? skyBit : 0) | (draw.wireframe ? wireframeBit : 0);
  }

  id<MTLRenderPipelineState> Raster::Gpu::pipeline(uint64_t key) {
    // Blending, write mask and target: the rest of the key is encoder state.
    const uint64_t pipelineKey = (key & ((uint64_t(1) << 27) - 1)) | (key & (skyBit | maskedBit | spriteBit));
    if (auto it = pipelines.find(pipelineKey); it != pipelines.end())
      return it->second;
    // Masked overlays test their group's coverage (raster/mask.cpp); sprite
    // overlays take the sprite stages (raster/sprite.cpp).
    const bool masked = key & maskedBit, sprite = key & spriteBit;
    auto *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = function(device, sprite ? shaders::Stage::spriteVert : shaders::Stage::rasterVert);
    descriptor.fragmentFunction = function(device, masked   ? shaders::Stage::maskOverlay
                                                   : sprite ? shaders::Stage::spriteFrag
                                                            : shaders::Stage::rasterFrag);
    auto *vertex = [MTLVertexDescriptor vertexDescriptor];
    vertex.attributes[0].format = MTLVertexFormatFloat3;
    vertex.attributes[0].offset = offsetof(Vertex, position);
    vertex.attributes[1].format = MTLVertexFormatFloat2;
    vertex.attributes[1].offset = offsetof(Vertex, uv);
    vertex.attributes[2].format = MTLVertexFormatFloat4;
    vertex.attributes[2].offset = offsetof(Vertex, color);
    for (unsigned i = 0; i < 3; ++i)
      vertex.attributes[i].bufferIndex = vertexBufferIndex;
    vertex.layouts[vertexBufferIndex].stride = sizeof(Vertex);
    if (sprite) {
      // The mesh's sprite vertices as a second buffer.
      vertex.attributes[3].format = MTLVertexFormatFloat3;
      vertex.attributes[3].offset = offsetof(SpriteVertex, frame);
      vertex.attributes[4].format = MTLVertexFormatFloat4;
      vertex.attributes[4].offset = offsetof(SpriteVertex, sequence);
      vertex.attributes[5].format = MTLVertexFormatFloat;
      vertex.attributes[5].offset = offsetof(SpriteVertex, weight2);
      for (unsigned i = 3; i < 6; ++i)
        vertex.attributes[i].bufferIndex = spriteBufferIndex;
      vertex.layouts[spriteBufferIndex].stride = sizeof(SpriteVertex);
    }
    descriptor.vertexDescriptor = vertex;
    auto *color = descriptor.colorAttachments[0];
    color.pixelFormat = key & skyBit ? skyFormat : displayFormat;
    color.blendingEnabled = (key >> 26) & 1;
    color.sourceRGBBlendFactor = blendFactor(key & 15);
    color.destinationRGBBlendFactor = blendFactor((key >> 4) & 15);
    color.rgbBlendOperation = blendOperation((key >> 8) & 7);
    color.sourceAlphaBlendFactor = blendFactor((key >> 11) & 15);
    color.destinationAlphaBlendFactor = blendFactor((key >> 15) & 15);
    color.alphaBlendOperation = blendOperation((key >> 19) & 7);
    color.writeMask = writeMask((key >> 22) & 15);
    descriptor.depthAttachmentPixelFormat = depthFormat;
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    check(state, "raster pipeline", error);
    return pipelines.emplace(pipelineKey, state).first->second;
  }

  id<MTLDepthStencilState> Raster::Gpu::depthState(uint64_t key) {
    const uint64_t depthKey = (key >> 28) & 31;
    if (auto it = depthStates.find(depthKey); it != depthStates.end())
      return it->second;
    auto *descriptor = [MTLDepthStencilDescriptor new];
    const bool test = depthKey & 1;
    descriptor.depthCompareFunction = test ? MTLCompareFunction((depthKey >> 2) & 7) : MTLCompareFunctionAlways;
    descriptor.depthWriteEnabled = test && ((depthKey >> 1) & 1);
    id<MTLDepthStencilState> state = [device newDepthStencilStateWithDescriptor:descriptor];
    check(state, "depth state");
    return depthStates.emplace(depthKey, state).first->second;
  }

  // raster/rendering.cpp RasterRenderer::prepare.
  void Raster::prepare(bool tracing) {
    auto &g = *m_gpu;
    m_profile = {};
    g.uploadBytes = 0;
    // Dynamic textures (remixapi_UpdateTextureEXT): every updatable image is
    // brought to its texture's current revision before the frame samples it.
    for (auto &[key, value] : g.textures)
      if (const auto owner = value.owner.lock(); owner && owner->updates)
        g.refreshTexture(value, *owner);
    struct Finish {
      Profile &profile;
      const uint64_t &uploads;
      std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
      uint64_t skinning = 0;
      ~Finish() {
        profile.prepareMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        profile.skinningMilliseconds = tickMilliseconds(skinning);
        profile.uploadBytes = uploads;
      }
    } finish{m_profile, g.uploadBytes};
    g.commands.clear();
    g.retire(g.skin);
    g.skin = nil;
    for (auto it = g.materials.begin(); it != g.materials.end();) {
      if (it->second->owner.expired()) {
        g.retire(it->second->set.current());
        it = g.materials.erase(it);
      } else
        ++it;
    }
    for (auto it = g.textures.begin(); it != g.textures.end();) {
      if (it->second.owner.expired()) {
        g.retire(it->second.texture);
        it = g.textures.erase(it);
      } else
        ++it;
    }
    for (auto it = g.meshes.begin(); it != g.meshes.end();) {
      if (it->second.owner.expired()) {
        g.retire(it->second.data);
        it = g.meshes.erase(it);
      } else
        ++it;
    }
    for (auto it = m_paths.begin(); it != m_paths.end();)
      it = it->second.expired() ? m_paths.erase(it) : std::next(it);
    auto layer = [](const Draw &d) {
      return d.ui                                          ? 3
             : d.camera == REMIXAPI_CAMERA_TYPE_SKY        ? 0
             : d.camera == REMIXAPI_CAMERA_TYPE_VIEW_MODEL ? 2
                                                           : 1;
    };
    auto distance = [&](const Draw &d) {
      const auto mvp = multiply(m_cameras[d.camera], d.model);
      const auto &c = d.surface->center;
      const float w = mvp[3] * c[0] + mvp[7] * c[1] + mvp[11] * c[2] + mvp[15];
      const float z = mvp[2] * c[0] + mvp[6] * c[1] + mvp[10] * c[2] + mvp[14];
      const float depth = z / w;
      return std::isfinite(depth) ? depth : 0.0f;
    };
    // `order` (remixapi_InstanceInfoRasterOrderEXT): a blended draw the host
    // placed itself, after the layer's depth-sorted ones.
    struct Entry {
      int layer;
      bool blended;
      float distance;
      const Draw *draw;
      uint32_t order = 0;
    };
    std::vector<Entry> ordered;
    ordered.reserve(m_draws.size() + m_overlay.size() + m_uiDraws.size() + 1);
    std::deque<Draw> adjusted;
    for (const auto &draw : m_draws) {
      if (draw.rasterVisible && (!tracing || !draw.traced())) {
        const Draw *source = &draw;
        // Traced barycentric depth and raster interpolation can differ by ULPs.
        // Bias coplanar EQUAL feedback toward the camera while preserving occlusion.
        if (tracing && draw.depthCompare == 2) {
          auto &copy = adjusted.emplace_back(draw);
          copy.depthCompare = 3;
          copy.depthBias = true;
          source = &copy;
        }
        // A masked overlay is drawn after its layer's opaque draws, which
        // precede the coverage passes (their depth is what sources test). So
        // is a rasterized CUT instance, drawn outside its group's coverage.
        const bool late = draw.blend.enabled || draw.maskGroup || draw.cutGroup;
        const uint32_t order = draw.blend.enabled && !draw.maskGroup && !draw.cutGroup ? draw.rasterOrder : 0;
        ordered.push_back({layer(draw), late, late && !order ? distance(draw) : 0.f, source, order});
      }
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const Entry &a, const Entry &b) {
      if (a.layer != b.layer)
        return a.layer < b.layer;
      if (a.blended != b.blended)
        return !a.blended;
      if (a.order || b.order)
        return a.order && b.order ? a.order < b.order : !a.order;
      return a.blended && a.distance > b.distance;
    });
    if (m_tint[3] > 0) {
      Draw tint;
      tint.ui = true;
      tint.depth = false;
      tint.doubleSided = true;
      tint.blend.enabled = true;
      tint.canvasWidth = tint.canvasHeight = 1;
      tint.material = std::make_shared<Material>();
      tint.material->texture = texturePath(nullptr);
      std::copy_n(m_tint, 4, tint.material->tint);
      tint.surface = std::make_shared<Surface>();
      tint.surface->vertices = {{{0, 0, 0}, {0, 0}, {1, 1, 1, 1}},
                                {{1, 0, 0}, {0, 0}, {1, 1, 1, 1}},
                                {{1, 1, 0}, {0, 0}, {1, 1, 1, 1}},
                                {{0, 1, 0}, {0, 0}, {1, 1, 1, 1}}};
      tint.surface->indices = {0, 1, 2, 0, 2, 3};
      ordered.push_back({0, false, 0, &adjusted.emplace_back(std::move(tint))});
    }
    for (const auto *list : {&m_overlay, &m_uiDraws})
      for (const auto &draw : *list)
        ordered.push_back({0, false, 0, &draw});
    m_profile.draws = ordered.size();
    // Any material binds a texture for the depth clear's fragment stage.
    g.clearMaterial = g.material(defaultMaterial()).set.current();
    std::vector<Vertex> skinned;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.epoch).count();
    g.sprite.soft = 0;
    g.sprite.captured = false;
    // A halo or mask source: drawn as a silhouette into a coverage target
    // instead of the frame.
    struct Silhouette {
      Gpu::Halo::Group *group; // counts the commands and bounds them on screen
      bool depthTest;          // honoured by WORLD instances only
      std::vector<Gpu::Command> *commands;
    };
    auto &masks = g.mask;
    const MTLScissorRect whole = {0, 0, g.width, g.height};
    // Coverage exists for this frame's masked draws (set below).
    bool masking = false;
    // Frustum culling (REMIX_RASTER_FRUSTUM_CULL=0 turns it off), as
    // raster/rendering.cpp: a draw whose index range lies outside the view is
    // left out. Not for skinned draws, sprite overlays, UI or silhouettes.
    static const bool cullEnabled = [] {
      const char *value = std::getenv("REMIX_RASTER_FRUSTUM_CULL");
      return !value || value[0] != '0';
    }();
    const auto outsideView = [&](const Draw &draw, Gpu::MeshGpu &mesh, const Matrix &m) {
      const Gpu::MeshGpu::RangeKey key = {draw.firstIndex,
                                          draw.indexCount ? draw.indexCount : uint32_t(draw.surface->indices.size()),
                                          draw.vertexOffset};
      auto found = mesh.ranges.find(key);
      if (found == mesh.ranges.end()) {
        Gpu::MeshGpu::Range range;
        const auto &indices = draw.surface->indices;
        const auto &vertices = draw.surface->vertices;
        range.valid = key.count && uint64_t(key.first) + key.count <= indices.size();
        std::fill_n(range.low, 3, std::numeric_limits<float>::max());
        std::fill_n(range.high, 3, std::numeric_limits<float>::lowest());
        for (uint32_t i = 0; i < key.count && range.valid; ++i) {
          const int64_t vertex = int64_t(indices[key.first + i]) + key.base;
          if (vertex < 0 || vertex >= int64_t(vertices.size())) {
            range.valid = false;
            break;
          }
          for (int c = 0; c < 3; ++c) {
            const float p = vertices[size_t(vertex)].position[c];
            range.valid = range.valid && std::isfinite(p);
            range.low[c] = std::min(range.low[c], p);
            range.high[c] = std::max(range.high[c], p);
          }
        }
        found = mesh.ranges.emplace(key, range).first;
      }
      const auto &range = found->second;
      if (!range.valid)
        return false;
      // Bits: outside left, right, bottom, top, behind the eye. A plane every
      // corner is outside of hides the box.
      uint32_t outside = 31;
      for (int corner = 0; corner < 8 && outside; ++corner) {
        const float p[3] = {corner & 1 ? range.high[0] : range.low[0], corner & 2 ? range.high[1] : range.low[1],
                            corner & 4 ? range.high[2] : range.low[2]};
        const float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
        const float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
        const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w))
          return false;
        const float edge = w + .01f * std::abs(w);
        outside &= uint32_t(x < -edge) | uint32_t(x > edge) << 1 | uint32_t(y < -edge) << 2 |
                   uint32_t(y > edge) << 3 | uint32_t(w <= 0) << 4;
      }
      return outside != 0;
    };
    // One command per draw.
    const auto build = [&](const Draw &draw, const Silhouette *silhouette) {
      const auto &mat = *draw.material;
      // A masked overlay samples its group's coverage; a group without
      // sources covers nothing. A rasterized CUT instance is an OUTSIDE draw
      // that is never left out: without coverage it is drawn whole, as is a
      // SKY one.
      const bool cut = !silhouette && !draw.maskGroup && draw.cutGroup;
      const uint32_t group = draw.maskGroup ? draw.maskGroup : draw.cutGroup;
      const bool outside = draw.maskOutside || cut;
      const bool masked = !silhouette && group;
      int32_t slot = masked && masking ? masks.slot[group] : -1;
      if (cut && draw.camera == REMIXAPI_CAMERA_TYPE_SKY)
        slot = -1;
      if (masked && !cut &&
          (!masking || draw.camera == REMIXAPI_CAMERA_TYPE_SKY || (slot < 0 && !outside) ||
           (slot >= 0 && !outside && !masks.area[slot].width)))
        return;
      Gpu::Command out = {};
      out.push.mvp = multiply(m_cameras[draw.camera], draw.model);
      if (!draw.ui && !m_cameraValid[draw.camera] && draw.camera != REMIXAPI_CAMERA_TYPE_WORLD)
        out.push.mvp = multiply(m_cameras[REMIXAPI_CAMERA_TYPE_WORLD], draw.model);
      auto &mesh = g.mesh(draw.surface);
      if (cullEnabled && !silhouette && !draw.ui && !draw.spriteFlags && !draw.surface->bonesPerVertex &&
          outsideView(draw, mesh, out.push.mvp)) {
        ++m_profile.culledDraws;
        return;
      }
      out.tested = silhouette && silhouette->depthTest && draw.camera == REMIXAPI_CAMERA_TYPE_WORLD;
      // A sprite overlay (raster/sprite.cpp): a blended draw that is not masked.
      const bool sprite = !silhouette && draw.spriteFlags && draw.blend.enabled && !draw.maskGroup &&
                          !draw.cutGroup && !draw.ui;
      auto &material = g.material(draw.material);
      out.material = material.set.current();
      out.texture = g.textures.at(draw.material->texture.get()).texture;
      out.pipeline = Gpu::key(draw, Target::Display) | (slot >= 0 ? maskedBit : 0) | (sprite ? spriteBit : 0);
      out.skyPipeline = Gpu::key(draw, Target::Sky) | (sprite ? spriteBit : 0);
      if (slot >= 0) {
        out.coverage = slot;
        out.area = outside ? whole : masks.area[slot];
      }
      out.vertices = out.indices = mesh.data;
      out.indexOffset = mesh.indexOffset;
      if (sprite) {
        out.sprite = true;
        // A mesh without sprite vertices: any data (its flags that read them are off).
        out.spriteOffset = draw.surface->sprite.empty() ? 0 : mesh.spriteOffset;
        const auto &blend = draw.blend;
        out.spriteParameters[0] = draw.spriteSoft[0] > 0 ? 1 / draw.spriteSoft[0] : 0;
        out.spriteParameters[1] = draw.spriteSoft[1];
        out.spriteParameters[2] = draw.spriteSoft[2];
        // MODULATE's k: dst x src + src x dst, or dst x src alone (VkBlendFactor DST_COLOR, SRC_COLOR).
        out.spriteParameters[3] = blend.src == 4 && blend.dst == 2 ? 2.f : 1.f;
      }
      // The captured depth is the world camera's.
      const uint32_t spriteFlags =
          !sprite ? 0u
                  : draw.spriteFlags & (draw.camera == REMIXAPI_CAMERA_TYPE_WORLD
                                            ? 0x1ffu
                                            : 0x1ffu & ~uint32_t(REMIXAPI_SPRITE_SOFT_DEPTH_EXT));
      g.sprite.soft += (spriteFlags & REMIXAPI_SPRITE_SOFT_DEPTH_EXT) != 0;
      out.firstIndex = draw.firstIndex;
      out.count = draw.indexCount ? draw.indexCount : uint32_t(draw.surface->indices.size());
      out.baseVertex = draw.vertexOffset;
      out.layer = layer(draw);
      out.doubleSided = draw.doubleSided;
      out.depthBias = draw.depthBias;
      const size_t firstSkinned = skinned.size();
      if (draw.surface->bonesPerVertex) {
        ScopedTicks skinTime{finish.skinning};
        m_profile.skinnedVertices += draw.surface->vertices.size();
        out.vertices = nil;
        out.vertexOffset = skinned.size() * sizeof(Vertex);
        if (skinned.size() + draw.surface->vertices.size() > 8 * 1024 * 1024)
          throw Error("skinned vertex limit exceeded");
        for (size_t v = 0; v < draw.surface->vertices.size(); ++v) {
          Vertex vertex = draw.surface->vertices[v];
          const auto original = vertex;
          std::fill_n(vertex.position, 3, 0.0f);
          for (uint32_t b = 0; b < draw.surface->bonesPerVertex; ++b) {
            const size_t index = v * draw.surface->bonesPerVertex + b;
            const auto &m = (*draw.bones)[draw.surface->bones[index]];
            for (int r = 0; r < 3; ++r) {
              vertex.position[r] += draw.surface->weights[index] *
                                    (m[r] * original.position[0] + m[4 + r] * original.position[1] +
                                     m[8 + r] * original.position[2] + m[12 + r]);
            }
          }
          skinned.push_back(vertex);
        }
      }
      if (draw.ui) {
        out.push.mvp = identity();
        out.push.mvp[0] = 2.0f / draw.canvasWidth;
        out.push.mvp[5] = 2.0f / draw.canvasHeight;
        out.push.mvp[12] = out.push.mvp[13] = -1;
      }
      std::copy_n(mat.tint, 4, out.push.tint);
      const uint32_t frame = uint32_t(std::fmod(seconds * mat.fps, uint32_t(mat.rows) * mat.columns));
      out.push.uv[0] = 1.0f / mat.columns;
      out.push.uv[1] = 1.0f / mat.rows;
      out.push.uv[2] = float(frame % mat.columns) / mat.columns;
      out.push.uv[3] = float(frame / mat.columns) / mat.rows;
      const auto &b = draw.blend;
      out.push.state[0] = (b.colorOp & 3u) | (b.colorOp & 4u) << 10 | (b.alphaOpTexture & 4u) << 11 |
                          b.color1 << 2 | b.color2 << 4 | (b.alphaOpTexture & 3u) << 6 | b.alpha1 << 8 |
                          b.alpha2 << 10;
      out.push.state[1] = draw.ignoreAlpha ? 7 : draw.alphaOverride ? draw.compare : mat.compare;
      const float reference = draw.alphaOverride ? draw.reference : mat.reference;
      std::memcpy(&out.push.state[2], &reference, sizeof(reference));
      // Sky draws into the tracer's linear sky image never encode (state.w
      // bit 1, cleared by record); bit 4: drawn outside the coverage.
      out.push.state[3] = uint32_t(draw.ignoreAlpha) | (g.encodeOutput ? 2u : 0u) |
                          (slot >= 0 && outside ? 16u : 0u) | spriteFlags << 5;
      out.push.factor[0] = linear(float((b.factor >> 16) & 255) / 255);
      out.push.factor[1] = linear(float((b.factor >> 8) & 255) / 255);
      out.push.factor[2] = linear(float(b.factor & 255) / 255);
      out.push.factor[3] = float(b.factor >> 24) / 255;
      if (!silhouette) {
        g.commands.push_back(out);
        return;
      }
      auto *bounds = silhouette->group;
      // Screen bounds of the silhouette, from the vertices' bounding box.
      float low[3], high[3];
      const auto extend = [&](const Vertex *vertices, size_t count, float *lo, float *hi) {
        std::fill_n(lo, 3, std::numeric_limits<float>::max());
        std::fill_n(hi, 3, std::numeric_limits<float>::lowest());
        for (size_t v = 0; v < count; ++v)
          for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], vertices[v].position[c]);
            hi[c] = std::max(hi[c], vertices[v].position[c]);
          }
      };
      if (draw.surface->bonesPerVertex) {
        extend(skinned.data() + firstSkinned, skinned.size() - firstSkinned, low, high);
      } else {
        if (!mesh.bounded) {
          extend(draw.surface->vertices.data(), draw.surface->vertices.size(), mesh.low, mesh.high);
          mesh.bounded = true;
        }
        std::copy_n(mesh.low, 3, low);
        std::copy_n(mesh.high, 3, high);
      }
      bool bounded = low[0] <= high[0];
      float area[4] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
      const auto &m = out.push.mvp;
      for (int corner = 0; corner < 8 && bounded; ++corner) {
        const float p[3] = {corner & 1 ? high[0] : low[0], corner & 2 ? high[1] : low[1],
                            corner & 4 ? high[2] : low[2]};
        const float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
        const float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
        const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        // A box reaching behind the camera has no finite projection.
        if (!(w > 1e-4f) || !std::isfinite(x) || !std::isfinite(y)) {
          bounded = false;
          break;
        }
        area[0] = std::min(area[0], x / w);
        area[1] = std::min(area[1], y / w);
        area[2] = std::max(area[2], x / w);
        area[3] = std::max(area[3], y / w);
      }
      if (!bounded) {
        area[0] = area[1] = -1;
        area[2] = area[3] = 1;
      }
      if (!bounds->bounded) {
        std::copy_n(area, 2, bounds->low);
        std::copy_n(area + 2, 2, bounds->high);
        bounds->bounded = true;
      } else
        for (int c = 0; c < 2; ++c) {
          bounds->low[c] = std::min(bounds->low[c], area[c]);
          bounds->high[c] = std::max(bounds->high[c], area[2 + c]);
        }
      silhouette->commands->push_back(out);
      ++bounds->count;
    };
    // Mask sources (raster/mask.cpp): a coverage target per group and a pass
    // per run. Before the frame's draws, which need to know whether their
    // group has coverage.
    masks.runs.clear();
    masks.commands.clear();
    masks.used = 0;
    masks.cut = {};
    std::fill(std::begin(masks.slot), std::end(masks.slot), -1);
    if (!m_masks.empty()) {
      g.prepareSilhouettes();
      masking = true;
      struct Plan {
        const MaskRun *run;
        Gpu::Mask::Run pass;
      };
      std::vector<Plan> plans;
      for (const auto &run : m_masks) {
        Gpu::Mask::Run pass;
        if (run.mask.within) {
          // Limited to a group without coverage so far: covers nothing.
          pass.within = masks.slot[run.mask.within];
          if (pass.within < 0)
            continue;
        }
        auto &slot = masks.slot[run.mask.group];
        pass.clear = slot < 0;
        if (pass.clear)
          slot = int32_t(masks.used++);
        pass.target = uint32_t(slot);
        plans.push_back({&run, pass});
      }
      while (masks.targets.size() < masks.used)
        masks.targets.push_back(g.coverageTarget(false));
      const auto right = [](const MTLScissorRect &r) { return r.x + r.width; };
      const auto bottom = [](const MTLScissorRect &r) { return r.y + r.height; };
      for (auto &plan : plans) {
        Gpu::Halo::Group bounds;
        bounds.first = uint32_t(masks.commands.size());
        const Silhouette silhouette = {&bounds, plan.run->mask.depthTest, &masks.commands};
        for (const auto &draw : plan.run->draws)
          build(draw, &silhouette);
        plan.pass.first = bounds.first;
        plan.pass.count = bounds.count;
        masks.runs.push_back(plan.pass);
        // The pixels the group's coverage can be set in: masked INSIDE draws
        // are confined to them.
        const float margin[2] = {1, 1};
        MTLScissorRect area = {};
        if (g.coverageArea(bounds, margin))
          area = bounds.area;
        if (plan.pass.within >= 0 && area.width) {
          const auto &limit = masks.area[plan.pass.within];
          const NSUInteger x0 = std::max(area.x, limit.x), y0 = std::max(area.y, limit.y);
          const NSUInteger x1 = std::min(right(area), right(limit)), y1 = std::min(bottom(area), bottom(limit));
          area = x1 > x0 && y1 > y0 && limit.width ? MTLScissorRect{x0, y0, x1 - x0, y1 - y0} : MTLScissorRect{};
        }
        auto &total = masks.area[plan.pass.target];
        if (plan.pass.clear || !total.width)
          total = area;
        else if (area.width) {
          const NSUInteger x0 = std::min(area.x, total.x), y0 = std::min(area.y, total.y);
          const NSUInteger x1 = std::max(right(area), right(total)), y1 = std::max(bottom(area), bottom(total));
          total = {x0, y0, x1 - x0, y1 - y0};
        }
      }
      m_profile.maskGroups = masks.used;
      m_profile.maskDraws = masks.commands.size();
      // Role CUT with a traced scene: the groups that hide traced instances
      // from the camera and the runs their coverage needs, drawn ahead of the
      // path tracer as well (recordMasks early).
      if (tracing && m_cutGroups && !plans.empty()) {
        auto &cut = masks.cut;
        uint32_t early = 0; // groups, bit group - 1
        for (uint32_t group = 1; group <= Mask::maxGroup && cut.count < Mask::maxCutGroups; ++group) {
          const int32_t target = masks.slot[group];
          if (!(m_cutGroups >> (group - 1) & 1) || target < 0 || !masks.area[target].width)
            continue;
          early |= 1u << (group - 1);
          cut.targets[cut.count] = uint32_t(target);
          cut.slots[(group - 1) >> 3] |= (cut.count + 1) << (((group - 1) & 7) * 4);
          ++cut.count;
        }
        // And the groups those are limited to, transitively.
        for (bool grown = early != 0; grown;) {
          grown = false;
          for (const auto &plan : plans) {
            const auto &mask = plan.run->mask;
            if ((early >> (mask.group - 1) & 1) && mask.within && !(early >> (mask.within - 1) & 1)) {
              early |= 1u << (mask.within - 1);
              grown = true;
            }
          }
        }
        // The tracer reuses its output while its inputs are unchanged: this
        // is what changes when the coverage may have.
        uint64_t digest = hashBytes(cut.slots, sizeof(cut.slots));
        for (size_t i = 0; i < plans.size(); ++i) {
          const auto &run = *plans[i].run;
          if (!(early >> (run.mask.group - 1) & 1))
            continue;
          auto &pass = masks.runs[i];
          pass.early = true;
          const uint32_t parameters[] = {run.mask.group, run.mask.within, pass.count};
          digest = hashBytes(parameters, sizeof(parameters), digest);
          for (uint32_t c = pass.first; c < pass.first + pass.count; ++c) {
            const auto &source = masks.commands[c];
            digest = hashBytes(&source.push, sizeof(source.push), digest);
            const uint64_t values[] = {source.firstIndex, source.count, uint64_t(int64_t(source.baseVertex))};
            digest = hashBytes(values, sizeof(values), digest);
          }
          for (const auto &draw : run.draws) {
            const uintptr_t identity[] = {reinterpret_cast<uintptr_t>(draw.surface.get()),
                                          reinterpret_cast<uintptr_t>(draw.material.get())};
            digest = hashBytes(identity, sizeof(identity), digest);
            if (draw.bones)
              digest = hashBytes(draw.bones->data(), draw.bones->size() * sizeof(Matrix), digest);
          }
        }
        cut.digest = digest;
      }
    }
    for (const auto &entry : ordered)
      build(*entry.draw, nullptr);
    // The coverage passes run before the first masked world draw, or after
    // the world layer.
    masks.split = g.commands.size();
    if (!masks.runs.empty())
      for (size_t i = 0; i < g.commands.size(); ++i)
        if (g.commands[i].layer >= 2 || (g.commands[i].layer == 1 && g.commands[i].coverage >= 0)) {
          masks.split = i;
          break;
        }
    // Halo sources: one group of silhouette commands per halo that reaches
    // the screen.
    g.halo.groups.clear();
    g.halo.commands.clear();
    if (!m_halos.empty()) {
      g.prepareHalo();
      for (const auto &source : m_halos) {
        Gpu::Halo::Group group;
        group.halo = source.halo;
        group.first = uint32_t(g.halo.commands.size());
        const Silhouette silhouette = {&group, !source.halo.ignoreZ, &g.halo.commands};
        for (const auto &draw : source.draws)
          build(draw, &silhouette);
        m_profile.haloDraws += group.count;
        if (g.haloArea(group))
          g.halo.groups.push_back(group);
      }
      m_profile.haloGroups = g.halo.groups.size();
      m_profile.draws += m_profile.haloDraws;
    }
    m_profile.draws += m_profile.maskDraws;
    if (!skinned.empty()) {
      g.skin = buffer(g.device, skinned.data(), skinned.size() * sizeof(Vertex));
      g.uploadBytes += skinned.size() * sizeof(Vertex);
      for (auto *list : {&g.commands, &g.halo.commands, &masks.commands})
        for (auto &command : *list)
          if (!command.vertices)
            command.vertices = g.skin;
    }
  }

  // vkCmdClearAttachments of the depth attachment inside the pass: a
  // full-screen triangle at the far plane that writes only depth.
  void Raster::Gpu::clearDepth(id<MTLRenderCommandEncoder> encoder) {
    if (!clearGeometry) {
      const Vertex vertices[3] = {{{-1, -1, 1}, {0, 0}, {1, 1, 1, 1}},
                                  {{3, -1, 1}, {0, 0}, {1, 1, 1, 1}},
                                  {{-1, 3, 1}, {0, 0}, {1, 1, 1, 1}}};
      clearGeometry = buffer(device, vertices, sizeof(vertices));
    }
    Draw clear;
    clear.doubleSided = true;
    clear.blend.mask = 0;
    clear.depthWriteOverride = true;
    clear.depthCompare = 7; // always
    const auto clearKey = key(clear, Target::Display);
    RasterPush push{identity()};
    std::fill_n(push.tint, 4, 1.f);
    push.state[1] = 7; // alpha test passes
    [encoder setRenderPipelineState:pipeline(clearKey)];
    [encoder setDepthStencilState:depthState(clearKey)];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setTriangleFillMode:MTLTriangleFillModeFill];
    [encoder setDepthBias:0 slopeScale:0 clamp:0];
    [encoder setVertexBuffer:clearGeometry offset:0 atIndex:vertexBufferIndex];
    [encoder setVertexBytes:&push length:sizeof(push) atIndex:library(shaders::Stage::rasterVert).push];
    [encoder setFragmentBuffer:clearMaterial offset:0 atIndex:0];
    [encoder setFragmentBytes:&push length:sizeof(push) atIndex:library(shaders::Stage::rasterFrag).push];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }

  // raster/rendering.cpp RasterRenderer::record.
  void Raster::Gpu::record(id<MTLRenderCommandEncoder> encoder, Target target, bool skipSky, const float *skyJitter,
                           uint32_t w, uint32_t h, int firstLayer, int lastLayer, int part) {
    const bool skyOnly = target == Target::Sky;
    [encoder setViewport:{0, 0, double(w), double(h), 0, 1}];
    // Vulkan's clockwise front faces, in the shared Y-down framebuffer space.
    [encoder setFrontFacingWinding:MTLWindingClockwise];
    const MTLScissorRect whole = {0, 0, w, h};
    [encoder setScissorRect:whole];
    MTLScissorRect confined = whole;
    const auto same = [](const MTLScissorRect &a, const MTLScissorRect &b) {
      return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    };
    const size_t split = std::min(mask.split, commands.size());
    const size_t begin = part == 2 ? split : 0, end = part == 1 ? split : commands.size();
    // What this call has bound: consecutive draws mostly share it (the parts
    // of one mesh, the draws of one material).
    struct Bound {
      uint64_t key = ~uint64_t(0);
      id<MTLBuffer> material = nil, coverage = nil, vertices = nil, sprite = nil;
      size_t vertexOffset = 0, spriteOffset = 0;
      id<MTLTexture> texture = nil;
      float blend[4] = {};
      bool blendSet = false;
    } bound;
    int previousLayer = -1;
    for (size_t index = begin; index < end; ++index) {
      const auto &draw = commands[index];
      if ((skyOnly && draw.layer != 0) || (skipSky && draw.layer == 0) || draw.layer < firstLayer ||
          draw.layer > lastLayer)
        continue;
      if (draw.layer != previousLayer && draw.layer >= 2) {
        if (!same(confined, whole)) {
          [encoder setScissorRect:whole];
          confined = whole;
        }
        clearDepth(encoder);
        bound = {};
      }
      previousLayer = draw.layer;
      // A masked overlay is confined to the pixels its coverage can be set in.
      const auto &wanted = draw.coverage >= 0 ? draw.area : whole;
      if (!same(wanted, confined)) {
        [encoder setScissorRect:wanted];
        confined = wanted;
      }
      const uint64_t key = skyOnly ? draw.skyPipeline : draw.pipeline;
      if (key != bound.key) {
        [encoder setRenderPipelineState:pipeline(key)];
        [encoder setDepthStencilState:depthState(key)];
        [encoder setCullMode:draw.doubleSided ? MTLCullModeNone : MTLCullModeBack];
        [encoder setTriangleFillMode:key & wireframeBit ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        if (draw.depthBias)
          [encoder setDepthBias:-1 slopeScale:-1 clamp:0];
        else
          [encoder setDepthBias:0 slopeScale:0 clamp:0];
        bound.key = key;
      }
      if (!bound.blendSet || std::memcmp(bound.blend, draw.push.factor, sizeof(bound.blend))) {
        [encoder setBlendColorRed:draw.push.factor[0]
                            green:draw.push.factor[1]
                             blue:draw.push.factor[2]
                            alpha:draw.push.factor[3]];
        std::copy_n(draw.push.factor, 4, bound.blend);
        bound.blendSet = true;
      }
      const auto vertexStage = draw.sprite ? shaders::Stage::spriteVert : shaders::Stage::rasterVert;
      const auto fragmentStage = draw.coverage >= 0 ? shaders::Stage::maskOverlay
                                 : draw.sprite      ? shaders::Stage::spriteFrag
                                                    : shaders::Stage::rasterFrag;
      auto push = draw.push;
      // A sprite overlay: the scene's depth as set 1 (its own texture stands
      // in while there is none), its sprite vertices as a second buffer.
      const bool soft = draw.sprite && sprite.captured && sprite.depth;
      if (draw.sprite) {
        std::copy_n(draw.spriteParameters, 4, push.factor);
        if (!soft)
          push.state[3] &= ~(REMIXAPI_SPRITE_SOFT_DEPTH_EXT << 5);
      }
      if (skyJitter && draw.layer == 0) {
        // Match the primary ray sample; raster overlays keep their center sample.
        for (int column = 0; column < 4; ++column) {
          push.mvp[column * 4] -= 2 * skyJitter[0] / w * push.mvp[column * 4 + 3];
          push.mvp[column * 4 + 1] -= 2 * skyJitter[1] / h * push.mvp[column * 4 + 3];
        }
      }
      if (skyOnly)
        push.state[3] &= ~2u;
      if (draw.vertices != bound.vertices || draw.vertexOffset != bound.vertexOffset) {
        [encoder setVertexBuffer:draw.vertices offset:draw.vertexOffset atIndex:vertexBufferIndex];
        bound.vertices = draw.vertices;
        bound.vertexOffset = draw.vertexOffset;
      }
      if (draw.sprite && (draw.indices != bound.sprite || draw.spriteOffset != bound.spriteOffset)) {
        [encoder setVertexBuffer:draw.indices offset:draw.spriteOffset atIndex:spriteBufferIndex];
        bound.sprite = draw.indices;
        bound.spriteOffset = draw.spriteOffset;
      }
      [encoder setVertexBytes:&push length:sizeof(push) atIndex:library(vertexStage).push];
      if (draw.material != bound.material) {
        [encoder setFragmentBuffer:draw.material offset:0 atIndex:0];
        bound.material = draw.material;
      }
      if (draw.texture != bound.texture) {
        [encoder useResource:draw.texture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
        bound.texture = draw.texture;
      }
      id<MTLBuffer> second = nil;
      if (draw.coverage >= 0) {
        const auto &coverage = mask.targets[size_t(draw.coverage)];
        second = coverage.overlay.current();
        if (second != bound.coverage)
          [encoder useResource:coverage.texture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
      } else if (draw.sprite) {
        second = soft ? sprite.set.current() : draw.material;
        if (soft && second != bound.coverage)
          [encoder useResource:sprite.depth usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
      }
      if (second && second != bound.coverage) {
        [encoder setFragmentBuffer:second offset:0 atIndex:1];
        bound.coverage = second;
      }
      // raster.frag's push constants take the buffer index of the other
      // stages' set 1.
      const uint32_t fragmentPush = library(fragmentStage).push;
      [encoder setFragmentBytes:&push length:sizeof(push) atIndex:fragmentPush];
      if (fragmentPush == 1)
        bound.coverage = nil;
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:draw.count
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:draw.indices
                   indexBufferOffset:draw.indexOffset + size_t(draw.firstIndex) * sizeof(uint32_t)
                       instanceCount:1
                          baseVertex:draw.baseVertex
                        baseInstance:0];
    }
    if (!same(confined, whole))
      [encoder setScissorRect:whole];
    // What follows in the encoder (the UI) fills its triangles.
    [encoder setTriangleFillMode:MTLTriangleFillModeFill];
  }
} // namespace remix::metal
