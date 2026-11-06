// Host passes on Metal: runtime/host_passes.cpp itself (validation,
// submission, uploads, statistics), with the passes replayed here. The host's
// SPIR-V is translated to MSL by SPIRV-Cross when it is registered; a pass
// draws, copies or clears directly into the frame's texture or a host target.
#include "../runtime/host_passes.cpp"
#include "metal/gpu.h"
#include "metal/timers.h"
#include <spirv_msl.hpp>
#include <SDL3/SDL.h>
#include <iterator>

namespace remix {
  namespace {
    // A stage's Metal slots: vertex buffers 0-3, the push constant block 4,
    // uniform buffers from 8; textures and their samplers from 0.
    constexpr uint32_t pushIndex = 4, bufferBase = 8, samplerLimit = 16, bufferLimit = 31;

    MTLPixelFormat pixelFormat(uint32_t vkFormat) {
      switch (vkFormat) {
      case 9: return MTLPixelFormatR8Unorm;          // VK_FORMAT_R8_UNORM
      case 16: return MTLPixelFormatRG8Unorm;        // VK_FORMAT_R8G8_UNORM
      case 37: case 43: return MTLPixelFormatRGBA8Unorm; // R8G8B8A8 UNORM, SRGB (raw)
      case 44: case 50: return MTLPixelFormatBGRA8Unorm; // B8G8R8A8 UNORM, SRGB (raw)
      case 64: return MTLPixelFormatRGB10A2Unorm;    // VK_FORMAT_A2B10G10R10_UNORM_PACK32
      case 76: return MTLPixelFormatR16Float;        // VK_FORMAT_R16_SFLOAT
      case 83: return MTLPixelFormatRG16Float;       // VK_FORMAT_R16G16_SFLOAT
      case 97: return MTLPixelFormatRGBA16Float;     // VK_FORMAT_R16G16B16A16_SFLOAT
      case 100: return MTLPixelFormatR32Float;       // VK_FORMAT_R32_SFLOAT
      case 103: return MTLPixelFormatRG32Float;      // VK_FORMAT_R32G32_SFLOAT
      case 109: return MTLPixelFormatRGBA32Float;    // VK_FORMAT_R32G32B32A32_SFLOAT
      }
      return MTLPixelFormatInvalid;
    }

    // The frame's format as the VkFormat a host target of its format takes.
    uint32_t vkFormatOf(MTLPixelFormat format) {
      switch (format) {
      case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: return 37;
      case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB: return 44;
      case MTLPixelFormatRGB10A2Unorm: return 64;
      case MTLPixelFormatRGBA16Float: return 97;
      default: return 0;
      }
    }

    MTLVertexFormat vertexFormat(uint32_t vkFormat) {
      switch (vkFormat) {
      case 37: return MTLVertexFormatUChar4Normalized;      // R8G8B8A8_UNORM
      case 41: return MTLVertexFormatUChar4;                // R8G8B8A8_UINT
      case 44: return MTLVertexFormatUChar4Normalized_BGRA; // B8G8R8A8_UNORM
      case 77: return MTLVertexFormatShort2Normalized;      // R16G16_SNORM
      case 83: return MTLVertexFormatHalf2;                 // R16G16_SFLOAT
      case 84: return MTLVertexFormatUShort4Normalized;     // R16G16B16A16_UNORM
      case 97: return MTLVertexFormatHalf4;                 // R16G16B16A16_SFLOAT
      case 98: return MTLVertexFormatUInt;                  // R32_UINT
      case 100: return MTLVertexFormatFloat;                // R32_SFLOAT
      case 103: return MTLVertexFormatFloat2;               // R32G32_SFLOAT
      case 106: return MTLVertexFormatFloat3;               // R32G32B32_SFLOAT
      case 109: return MTLVertexFormatFloat4;               // R32G32B32A32_SFLOAT
      }
      return MTLVertexFormatInvalid;
    }

    // VkBlendFactor in MTLBlendFactor order.
    MTLBlendFactor blendFactor(uint32_t value) {
      static constexpr MTLBlendFactor factors[] = {
          MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
          MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor, MTLBlendFactorSourceAlpha,
          MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
          MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor, MTLBlendFactorBlendAlpha,
          MTLBlendFactorOneMinusBlendAlpha, MTLBlendFactorSourceAlphaSaturated, MTLBlendFactorSource1Color,
          MTLBlendFactorOneMinusSource1Color, MTLBlendFactorSource1Alpha, MTLBlendFactorOneMinusSource1Alpha};
      return value < std::size(factors) ? factors[value] : MTLBlendFactorZero;
    }

    MTLColorWriteMask writeMask(uint32_t vkMask) {
      // VkColorComponentFlags R1 G2 B4 A8; MTLColorWriteMask A1 B2 G4 R8.
      return MTLColorWriteMask(((vkMask & 1) ? MTLColorWriteMaskRed : 0) | ((vkMask & 2) ? MTLColorWriteMaskGreen : 0) |
                               ((vkMask & 4) ? MTLColorWriteMaskBlue : 0) | ((vkMask & 8) ? MTLColorWriteMaskAlpha : 0));
    }

    bool fits(const int32_t rect[4], uint32_t width, uint32_t height) {
      return rect[2] == 0 || (rect[0] >= 0 && rect[1] >= 0 && rect[2] > 0 && rect[3] > 0 &&
                              uint32_t(rect[0] + rect[2]) <= width && uint32_t(rect[1] + rect[3]) <= height);
    }

    // A scaled copy: a quad over the target rectangle sampling the source's.
    constexpr const char *copySource = R"(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 position [[position]]; float2 uv; };
vertex Out copyVertex(uint id [[vertex_id]], constant float4 &source [[buffer(0)]]) {
  float2 corner = float2(id & 1, id >> 1);
  Out out;
  out.position = float4(corner.x * 2 - 1, 1 - corner.y * 2, 0, 1);
  out.uv = source.xy + corner * source.zw;
  return out;
}
fragment float4 copyFragment(Out in [[stage_in]], texture2d<float> image [[texture(0)]], sampler s [[sampler(0)]]) {
  return image.sample(s, in.uv);
}
)";
  } // namespace

  struct HostPasses::Metal {
    id<MTLDevice> device = nil;
    struct Target {
      id<MTLTexture> texture = nil, sampled = nil; // sampled: swizzled as `channels` says
      uint32_t format = 0, channels = 0, depth = 1;
      bool initialized = false;
      uint64_t lastFrame = 0;
    };
    std::map<remixapi_HostTargetHandleEXT, Target> targets; // 0: the frame (its own texture), FRAME_DEPTH its depth
    std::map<std::string, id<MTLRenderPipelineState>> pipelines;
    std::map<uint32_t, id<MTLDepthStencilState>> depthStates;
    std::map<uint32_t, id<MTLSamplerState>> samplers;
    std::map<MTLPixelFormat, id<MTLRenderPipelineState>> copies;
    id<MTLLibrary> copyLibrary = nil;
    id<MTLBuffer> slots[gpuFrameCount] = {};
    size_t used = 0;

    Target &target(remixapi_HostTargetHandleEXT handle, uint32_t format, uint32_t width, uint32_t height,
                   uint32_t channels, uint64_t frame, uint32_t depth = 1);
    id<MTLSamplerState> sampler(const remixapi_HostPassSamplerEXT &s);
    id<MTLRenderPipelineState> copy(MTLPixelFormat format);
    size_t stage(id<MTLBuffer> buffer, const void *data, size_t bytes, size_t alignment) {
      const size_t offset = (used + alignment - 1) / alignment * alignment;
      if (offset + bytes > buffer.length)
        throw std::runtime_error("host pass staging overflow");
      std::memcpy(static_cast<uint8_t *>(buffer.contents) + offset, data, bytes);
      used = offset + bytes;
      return offset;
    }
  };

  void HostPasses::initialize(void *device) {
    m_metal = std::make_shared<Metal>();
    m_metal->device = (__bridge id<MTLDevice>)device;
  }

  void HostPasses::destroy() {
    m_metal.reset();
    std::lock_guard lock(m_mutex);
    m_shaders.clear();
    m_pending.clear();
  }

  bool HostPasses::ready() const { return m_metal != nullptr; }

  // The host's SPIR-V as MSL: set 0's bindings at this stage's Metal slots.
  bool HostPasses::createModule(Shader &shader) {
    try {
      using namespace remix_spirv_cross;
      CompilerMSL compiler(shader.code);
      CompilerMSL::Options options;
      options.platform = CompilerMSL::Options::macOS;
      options.set_msl_version(2, 4);
      options.pad_fragment_output_components = true;
      compiler.set_msl_options(options);
      const auto model = shader.vertex ? spv::ExecutionModelVertex : spv::ExecutionModelFragment;
      uint32_t buffers = bufferBase, textures = 0;
      for (const auto &binding : shader.bindings) {
        MSLResourceBinding resource;
        resource.stage = model;
        resource.desc_set = 0;
        resource.binding = binding.binding;
        if (binding.type == REMIXAPI_HOST_BINDING_UNIFORM_BUFFER_EXT) {
          if (buffers >= bufferLimit)
            return false;
          resource.msl_buffer = buffers;
          shader.slots[binding.binding] = buffers++;
        } else {
          if (textures >= samplerLimit)
            return false;
          resource.msl_texture = resource.msl_sampler = textures;
          shader.slots[binding.binding] = textures++;
        }
        compiler.add_msl_resource_binding(resource);
      }
      MSLResourceBinding push;
      push.stage = model;
      push.desc_set = kPushConstDescSet;
      push.binding = kPushConstBinding;
      push.msl_buffer = pushIndex;
      compiler.add_msl_resource_binding(push);
      const std::string source = compiler.compile();
      const std::string entry = compiler.get_cleansed_entry_point_name("main", model);
      auto *compile = [MTLCompileOptions new];
      compile.languageVersion = MTLLanguageVersion2_4;
      NSError *error = nil;
      id<MTLLibrary> library = [m_metal->device newLibraryWithSource:@(source.c_str()) options:compile error:&error];
      id<MTLFunction> function = library ? [library newFunctionWithName:@(entry.c_str())] : nil;
      if (!function) {
        SDL_Log("Host shader translation failed: %s", error ? error.localizedDescription.UTF8String : entry.c_str());
        return false;
      }
      shader.function = std::shared_ptr<void>((__bridge_retained void *)function, [](void *p) { CFRelease(p); });
      return true;
    } catch (const std::exception &error) {
      SDL_Log("Host shader translation failed: %s", error.what());
      return false;
    }
  }

  HostPasses::Metal::Target &HostPasses::Metal::target(remixapi_HostTargetHandleEXT handle, uint32_t format,
                                                      uint32_t width, uint32_t height, uint32_t channels,
                                                      uint64_t frame, uint32_t depth) {
    auto &t = targets[handle];
    if (t.texture && t.depth > 1 && depth == 1)
      return t; // a volume stays one: a pass naming it as its target is skipped
    if (t.texture && (t.format != format || t.texture.width != width || t.texture.height != height ||
                      t.channels != channels || t.depth != depth))
      t = {};
    if (!t.texture) {
      const auto pixel = pixelFormat(format);
      if (pixel == MTLPixelFormatInvalid)
        return t; // a format Metal is not given: passes on it are skipped
      auto *descriptor = [MTLTextureDescriptor new];
      descriptor.textureType = depth > 1 ? MTLTextureType3D : MTLTextureType2D;
      descriptor.pixelFormat = pixel;
      descriptor.width = width;
      descriptor.height = height;
      descriptor.depth = depth;
      descriptor.usage = MTLTextureUsageShaderRead | (depth > 1 ? 0 : MTLTextureUsageRenderTarget) |
                         MTLTextureUsagePixelFormatView;
      descriptor.storageMode = MTLStorageModePrivate;
      t.texture = [device newTextureWithDescriptor:descriptor];
      metal::check(t.texture, "host target");
      t.texture.label = @"Host pass target";
      t.format = format;
      t.channels = channels;
      t.depth = depth;
      t.initialized = false;
      // What passes sample: luminance formats replicate red, alpha-less ones read one.
      const auto r = MTLTextureSwizzleRed, g = MTLTextureSwizzleGreen, b = MTLTextureSwizzleBlue,
                 a = MTLTextureSwizzleAlpha, one = MTLTextureSwizzleOne;
      MTLTextureSwizzleChannels swizzle = MTLTextureSwizzleChannelsMake(r, g, b, a);
      if (channels == REMIXAPI_HOST_CHANNELS_LUMINANCE_EXT)
        swizzle = MTLTextureSwizzleChannelsMake(r, r, r, one);
      else if (channels == REMIXAPI_HOST_CHANNELS_LUMINANCE_ALPHA_EXT)
        swizzle = MTLTextureSwizzleChannelsMake(r, r, r, a);
      else if (channels == REMIXAPI_HOST_CHANNELS_NO_ALPHA_EXT)
        swizzle = MTLTextureSwizzleChannelsMake(r, g, b, one);
      t.sampled = channels ? [t.texture newTextureViewWithPixelFormat:pixel
                                                          textureType:descriptor.textureType
                                                               levels:NSMakeRange(0, 1)
                                                               slices:NSMakeRange(0, 1)
                                                              swizzle:swizzle]
                           : t.texture;
    }
    t.lastFrame = frame;
    return t;
  }

  id<MTLSamplerState> HostPasses::Metal::sampler(const remixapi_HostPassSamplerEXT &s) {
    const uint32_t key = s.linear | s.addressU << 1 | s.addressV << 3;
    if (auto found = samplers.find(key); found != samplers.end())
      return found->second;
    const MTLSamplerAddressMode modes[] = {MTLSamplerAddressModeRepeat, MTLSamplerAddressModeClampToEdge,
                                           MTLSamplerAddressModeMirrorRepeat};
    auto *descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = descriptor.magFilter = s.linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    descriptor.sAddressMode = modes[s.addressU];
    descriptor.tAddressMode = modes[s.addressV];
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> result = [device newSamplerStateWithDescriptor:descriptor];
    metal::check(result, "host pass sampler");
    return samplers[key] = result;
  }

  id<MTLRenderPipelineState> HostPasses::Metal::copy(MTLPixelFormat format) {
    if (auto found = copies.find(format); found != copies.end())
      return found->second;
    NSError *error = nil;
    if (!copyLibrary)
      copyLibrary = [device newLibraryWithSource:@(copySource) options:nil error:&error];
    metal::check(copyLibrary, "host copy shader", error);
    auto *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.label = @"Host pass copy";
    descriptor.vertexFunction = [copyLibrary newFunctionWithName:@"copyVertex"];
    descriptor.fragmentFunction = [copyLibrary newFunctionWithName:@"copyFragment"];
    descriptor.colorAttachments[0].pixelFormat = format;
    id<MTLRenderPipelineState> result = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    metal::check(result, "host copy pipeline", error);
    return copies[format] = result;
  }

  void HostPasses::record(void *commandBuffer, uint32_t slotIndex, void *frameTexture, void *viewDepthTexture) {
    std::vector<Pass> passes;
    std::vector<Upload> uploads;
    {
      std::lock_guard lock(m_mutex);
      passes = std::move(m_pending);
      m_pending.clear();
      uploads = std::move(m_uploads);
      m_uploads.clear();
      m_stats.passes = uint32_t(passes.size());
    }
    if ((passes.empty() && uploads.empty()) || !m_metal)
      return;
    ++m_frame;
    auto &m = *m_metal;
    id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)commandBuffer;
    id<MTLTexture> frameImage = (__bridge id<MTLTexture>)frameTexture;
    id<MTLTexture> viewDepth = (__bridge id<MTLTexture>)viewDepthTexture;
    const uint32_t width = uint32_t(frameImage.width), height = uint32_t(frameImage.height);
    // The frame's staging: every pass's bytes, aligned for uniform buffers.
    size_t bytes = 0;
    for (const auto &pass : passes)
      bytes += pass.data.size() + pass.indices.size() * 4 + 256 * (pass.buffers.size() + pass.vertexBindings.size() + 2);
    for (const auto &upload : uploads)
      bytes += upload.pixels.size() + 256;
    auto &staging = m.slots[slotIndex % gpuFrameCount];
    if (!staging || staging.length < bytes)
      staging = metal::buffer(m.device, nullptr, std::max<size_t>(bytes + bytes / 2, 256 * 1024));
    m.used = 0;
    // The host's texture contents first: into their targets, through staging.
    for (const auto &upload : uploads) {
      auto &t = m.target(upload.target, upload.format, upload.width, upload.height, 0, m_frame, upload.depth);
      if (!t.texture)
        continue;
      const size_t offset = m.stage(staging, upload.pixels.data(), upload.pixels.size(), 16);
      id<MTLBlitCommandEncoder> blit = metal::blitEncoder(command, @"Host target upload");
      [blit copyFromBuffer:staging
                 sourceOffset:offset
            sourceBytesPerRow:size_t(upload.width) * 4
          sourceBytesPerImage:size_t(upload.width) * upload.height * 4
                   sourceSize:MTLSizeMake(upload.width, upload.height, upload.depth)
                    toTexture:t.texture
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
      [blit endEncoding];
      t.initialized = true;
    }
    // The frame is its own texture, in the format it is drawn in.
    auto &frame = m.targets[0];
    frame.texture = frame.sampled = frameImage;
    frame.initialized = true;
    const auto skip = [&](uint32_t &reason) {
      ++m_stats.skipped;
      ++reason;
    };
    const auto ensure = [&](remixapi_HostTargetHandleEXT handle, const remixapi_HostPassEXT &info) -> Metal::Target & {
      if (!handle)
        return frame;
      return m.target(handle, info.targetFormat ? info.targetFormat : vkFormatOf(frameImage.pixelFormat), info.targetWidth,
                      info.targetHeight, info.targetChannels, m_frame);
    };
    const auto frameDepth = [&]() -> Metal::Target & {
      auto &t = m.targets[REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT];
      if (!t.texture || t.texture.width != width || t.texture.height != height) {
        t = {};
        t.texture = t.sampled = metal::texture2d(m.device, MTLPixelFormatDepth32Float, width, height,
                                                 MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        t.texture.label = @"Host frame depth";
      }
      t.lastFrame = m_frame;
      return t;
    };
    const auto renderPass = [&](id<MTLTexture> color, id<MTLTexture> depth, bool clearColor, const float rgba[4],
                                bool clearDepth, float depthValue, NSString *label) {
      auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
      if (color) {
        pass.colorAttachments[0].texture = color;
        pass.colorAttachments[0].loadAction = clearColor ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (clearColor)
          pass.colorAttachments[0].clearColor = MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
      }
      if (depth) {
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = clearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = depthValue;
      }
      return metal::renderEncoder(command, pass, label, color ? uint32_t(color.width) : width,
                                  color ? uint32_t(color.height) : height);
    };
    for (const auto &pass : passes) {
      const auto &i = pass.info;
      if (i.kind == REMIXAPI_HOST_PASS_CLEAR_EXT && i.target == REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT) {
        auto &t = frameDepth();
        [renderPass(nil, t.texture, false, nullptr, true, i.clearColor[0], @"Host pass: depth clear") endEncoding];
        t.initialized = true;
        ++m_stats.clears;
        continue;
      }
      if (i.kind == REMIXAPI_HOST_PASS_CLEAR_EXT) {
        auto &t = ensure(i.target, i);
        if (!t.texture || t.depth > 1) {
          skip(m_stats.skippedTarget);
          continue;
        }
        [renderPass(t.texture, nil, true, i.clearColor, false, 0, @"Host pass: clear") endEncoding];
        t.initialized = true;
        ++m_stats.clears;
        continue;
      }
      if (i.kind == REMIXAPI_HOST_PASS_COPY_EXT) {
        auto &t = ensure(i.target, i);
        auto source = m.targets.find(i.source);
        if (!t.texture || t.depth > 1 || source == m.targets.end() || !source->second.initialized ||
            source->second.depth > 1 || !fits(i.sourceRect, uint32_t(source->second.texture.width), uint32_t(source->second.texture.height)) ||
            !fits(i.targetRect, uint32_t(t.texture.width), uint32_t(t.texture.height))) {
          skip(m_stats.skippedCopySource);
          continue;
        }
        source->second.lastFrame = m_frame;
        id<MTLTexture> from = source->second.texture;
        // Drawn from a copy when the target is its own source.
        if (from == t.texture) {
          auto &scratch = m.targets[~0ull];
          if (!scratch.texture || scratch.texture.width != from.width || scratch.texture.height != from.height ||
              scratch.texture.pixelFormat != from.pixelFormat)
            scratch.texture = scratch.sampled = metal::texture2d(m.device, from.pixelFormat, uint32_t(from.width),
                                                                 uint32_t(from.height), MTLTextureUsageShaderRead);
          id<MTLBlitCommandEncoder> blit = metal::blitEncoder(command, @"Host pass: copy source");
          [blit copyFromTexture:from toTexture:scratch.texture];
          [blit endEncoding];
          from = scratch.texture;
        }
        const float sw = float(from.width), sh = float(from.height);
        const float source4[4] = {i.sourceRect[2] ? i.sourceRect[0] / sw : 0.f, i.sourceRect[2] ? i.sourceRect[1] / sh : 0.f,
                                  i.sourceRect[2] ? i.sourceRect[2] / sw : 1.f, i.sourceRect[2] ? i.sourceRect[3] / sh : 1.f};
        id<MTLRenderCommandEncoder> encoder = renderPass(t.texture, nil, false, nullptr, false, 0, @"Host pass: copy");
        const double tx = i.targetRect[2] ? i.targetRect[0] : 0, ty = i.targetRect[2] ? i.targetRect[1] : 0;
        const double tw = i.targetRect[2] ? i.targetRect[2] : double(t.texture.width),
                     th = i.targetRect[2] ? i.targetRect[3] : double(t.texture.height);
        [encoder setViewport:{tx, ty, tw, th, 0, 1}];
        [encoder setRenderPipelineState:m.copy(t.texture.pixelFormat)];
        [encoder setVertexBytes:source4 length:sizeof(source4) atIndex:0];
        [encoder setFragmentTexture:from atIndex:0];
        remixapi_HostPassSamplerEXT filter{};
        filter.linear = i.linear;
        filter.addressU = filter.addressV = 1;
        [encoder setFragmentSamplerState:m.sampler(filter) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];
        t.initialized = true;
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
      if (!vertex || !fragment || !vertex->vertex || fragment->vertex) {
        skip(m_stats.skippedShaders);
        continue;
      }
      const bool depthOnly = i.target == REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT;
      auto &t = depthOnly ? frameDepth() : ensure(i.target, i);
      if (!t.texture || t.depth > 1) {
        skip(m_stats.skippedTarget); // a volume (sampled only) or a format Metal is not given
        continue;
      }
      id<MTLTexture> depth = nil;
      if (depthOnly)
        depth = t.texture;
      else if (!i.target && i.depth) {
        // A depth-tested draw on the frame: against the host's frame depth.
        auto found = m.targets.find(REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT);
        if (found == m.targets.end() || !found->second.initialized || found->second.texture.width != width ||
            found->second.texture.height != height) {
          skip(m_stats.skippedTarget);
          continue;
        }
        found->second.lastFrame = m_frame;
        depth = found->second.texture;
      }
      // The pipeline: everything it is made of.
      std::string key;
      const auto add = [&](const void *data, size_t size) { key.append(static_cast<const char *>(data), size); };
      const void *functions[] = {vertex->function.get(), fragment->function.get()};
      add(functions, sizeof(functions));
      const MTLPixelFormat colorFormat = depthOnly ? MTLPixelFormatInvalid : t.texture.pixelFormat;
      add(&colorFormat, sizeof(colorFormat));
      const bool hasDepth = depth != nil;
      add(&hasDepth, sizeof(hasDepth));
      add(&i.blendEnable, sizeof(uint32_t) * 8); // blendEnable through colorWriteMask
      for (const auto &v : pass.vertexBindings)
        add(&v.stride, sizeof(v.stride));
      for (const auto &a : pass.attributes)
        add(&a, sizeof(a));
      id<MTLRenderPipelineState> pipeline = nil;
      if (auto found = m.pipelines.find(key); found != m.pipelines.end())
        pipeline = found->second;
      else {
        auto *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.label = @"Host pass";
        descriptor.vertexFunction = (__bridge id<MTLFunction>)vertex->function.get();
        descriptor.fragmentFunction = (__bridge id<MTLFunction>)fragment->function.get();
        auto *vertices = [MTLVertexDescriptor vertexDescriptor];
        bool formats = true;
        for (const auto &a : pass.attributes) {
          const auto format = vertexFormat(a.format);
          formats &= format != MTLVertexFormatInvalid;
          vertices.attributes[a.location].format = format;
          vertices.attributes[a.location].offset = a.offset;
          vertices.attributes[a.location].bufferIndex = a.binding;
        }
        for (uint32_t b = 0; b < pass.vertexBindings.size(); ++b)
          vertices.layouts[b].stride = pass.vertexBindings[b].stride;
        descriptor.vertexDescriptor = vertices;
        if (!depthOnly) {
          auto *color = descriptor.colorAttachments[0];
          color.pixelFormat = colorFormat;
          color.blendingEnabled = i.blendEnable != 0;
          color.sourceRGBBlendFactor = blendFactor(i.srcColorBlend);
          color.destinationRGBBlendFactor = blendFactor(i.dstColorBlend);
          color.rgbBlendOperation = MTLBlendOperation(i.colorBlendOp); // VkBlendOp 0-4 in Metal's order
          color.sourceAlphaBlendFactor = blendFactor(i.srcAlphaBlend);
          color.destinationAlphaBlendFactor = blendFactor(i.dstAlphaBlend);
          color.alphaBlendOperation = MTLBlendOperation(i.alphaBlendOp);
          color.writeMask = writeMask(i.colorWriteMask);
        }
        if (hasDepth)
          descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        NSError *error = nil;
        pipeline = formats ? [m.device newRenderPipelineStateWithDescriptor:descriptor error:&error] : nil;
        if (!pipeline && error)
          SDL_Log("Host pass pipeline: %s", error.localizedDescription.UTF8String);
        m.pipelines.emplace(std::move(key), pipeline);
        std::lock_guard lock(m_mutex);
        m_stats.pipelines = uint32_t(m.pipelines.size());
      }
      if (!pipeline) {
        skip(m_stats.skippedPipeline);
        continue;
      }
      // The pass's bytes, staged in the order submit copied them.
      size_t dataOffset = 0;
      std::vector<size_t> bufferOffsets(pass.buffers.size());
      for (size_t b = 0; b < pass.buffers.size(); ++b) {
        bufferOffsets[b] = m.stage(staging, pass.data.data() + dataOffset, pass.buffers[b].bytes, 256);
        dataOffset += pass.buffers[b].bytes;
      }
      const uint8_t *push = pass.data.data() + dataOffset;
      dataOffset += i.pushConstantBytes;
      std::vector<size_t> vertexOffsets(pass.vertexBindings.size());
      for (size_t v = 0; v < pass.vertexBindings.size(); ++v) {
        vertexOffsets[v] = m.stage(staging, pass.data.data() + dataOffset, pass.vertexBindings[v].bytes, 16);
        dataOffset += pass.vertexBindings[v].bytes;
      }
      const size_t indexOffset = m.stage(staging, pass.indices.data(), pass.indices.size() * 4, 4);
      // Each stage's bindings: a buffer or sampled texture for every one.
      struct Bound {
        bool vertexStage;
        uint32_t slot;
        id<MTLBuffer> buffer;
        size_t offset;
        id<MTLTexture> texture;
        id<MTLSamplerState> sampler;
      };
      std::vector<Bound> bound;
      bool complete = true;
      uint32_t *incomplete = &m_stats.skippedBuffer;
      for (const Shader *shader : {vertex.get(), fragment.get()}) {
        for (const auto &binding : shader->bindings) {
          const uint32_t slot = shader->slots.at(binding.binding);
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
            bound.push_back({shader->vertex, slot, staging, bufferOffsets[found], nil, nil});
            continue;
          }
          const remixapi_HostPassSamplerEXT *found = nullptr;
          for (const auto &s : pass.samplers)
            if (s.binding == binding.binding)
              found = &s;
          if (!found) {
            complete = false;
            incomplete = &m_stats.skippedSampler;
            break;
          }
          id<MTLTexture> sampled = frame.sampled;
          if (found->target == REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT) {
            // The runtime's own image, when the frame has it.
            if (!viewDepth) {
              complete = false;
              incomplete = &m_stats.skippedViewDepth;
              break;
            }
            sampled = viewDepth;
          } else if (found->target) {
            auto it = m.targets.find(found->target);
            if (it == m.targets.end() || !it->second.initialized) {
              complete = false;
              incomplete = &m_stats.skippedTarget;
              break;
            }
            sampled = it->second.sampled;
            it->second.lastFrame = m_frame;
          } else if (!i.target) {
            // Sampling the frame while drawing it: through a copy of it.
            auto &scratch = m.targets[~0ull];
            if (!scratch.texture || scratch.texture.width != width || scratch.texture.height != height ||
                scratch.texture.pixelFormat != frameImage.pixelFormat)
              scratch.texture = scratch.sampled =
                  metal::texture2d(m.device, frameImage.pixelFormat, width, height, MTLTextureUsageShaderRead);
            id<MTLBlitCommandEncoder> blit = metal::blitEncoder(command, @"Host pass: frame copy");
            [blit copyFromTexture:frameImage toTexture:scratch.texture];
            [blit endEncoding];
            sampled = scratch.texture;
          }
          bound.push_back({shader->vertex, slot, nil, 0, sampled, m.sampler(*found)});
        }
        if (!complete)
          break;
      }
      if (!complete) {
        skip(*incomplete);
        continue;
      }
      id<MTLRenderCommandEncoder> encoder =
          renderPass(depthOnly ? nil : t.texture, depth, false, nullptr, false, 0, @"Host pass: draw");
      [encoder setRenderPipelineState:pipeline];
      if (i.depth) {
        const uint32_t state = i.depth;
        auto &d = m.depthStates[state];
        if (!d) {
          auto *descriptor = [MTLDepthStencilDescriptor new];
          descriptor.depthCompareFunction = MTLCompareFunction((state & 0xff) - 1); // VkCompareOp in Metal's order
          descriptor.depthWriteEnabled = (state & REMIXAPI_HOST_DEPTH_WRITE_EXT) != 0;
          d = [m.device newDepthStencilStateWithDescriptor:descriptor];
        }
        [encoder setDepthStencilState:d];
      }
      [encoder setCullMode:MTLCullModeNone];
      // D3D9's viewport (y down) as Metal's, with D3D9's pixel centres half a
      // pixel before (a little less, as DXVK does, to keep edges off centres).
      const uint32_t tw = uint32_t(t.texture.width), th = uint32_t(t.texture.height);
      const double vx = i.viewport[2] ? i.viewport[0] : 0, vy = i.viewport[2] ? i.viewport[1] : 0;
      const double vw = i.viewport[2] ? i.viewport[2] : tw, vh = i.viewport[2] ? i.viewport[3] : th;
      constexpr double half = .5 - 1. / 128;
      [encoder setViewport:{vx + half, vy + half, vw, vh, 0, 1}];
      MTLScissorRect scissor{0, 0, tw, th};
      if (i.scissor[2] && fits(i.scissor, tw, th))
        scissor = {NSUInteger(i.scissor[0]), NSUInteger(i.scissor[1]), NSUInteger(i.scissor[2]), NSUInteger(i.scissor[3])};
      [encoder setScissorRect:scissor];
      for (const auto &b : bound) {
        if (b.buffer) {
          if (b.vertexStage)
            [encoder setVertexBuffer:b.buffer offset:b.offset atIndex:b.slot];
          else
            [encoder setFragmentBuffer:b.buffer offset:b.offset atIndex:b.slot];
        } else if (b.vertexStage) {
          [encoder setVertexTexture:b.texture atIndex:b.slot];
          [encoder setVertexSamplerState:b.sampler atIndex:b.slot];
        } else {
          [encoder setFragmentTexture:b.texture atIndex:b.slot];
          [encoder setFragmentSamplerState:b.sampler atIndex:b.slot];
        }
      }
      const uint32_t pushBytes = std::max(vertex->pushBytes, fragment->pushBytes);
      if (pushBytes) {
        uint8_t block[128] = {};
        std::memcpy(block, push, std::min<uint32_t>(i.pushConstantBytes, pushBytes));
        [encoder setVertexBytes:block length:pushBytes atIndex:pushIndex];
        [encoder setFragmentBytes:block length:pushBytes atIndex:pushIndex];
      }
      for (uint32_t v = 0; v < pass.vertexBindings.size(); ++v)
        [encoder setVertexBuffer:staging offset:vertexOffsets[v] atIndex:v];
      [encoder drawIndexedPrimitives:i.topology ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle
                          indexCount:pass.indices.size()
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:staging
                   indexBufferOffset:indexOffset];
      [encoder endEncoding];
      t.initialized = true;
      ++m_stats.draws;
    }
    // Targets the host stopped using go after a while.
    for (auto it = m.targets.begin(); it != m.targets.end();) {
      if (it->first && it->first < REMIXAPI_HOST_TARGET_RESERVED_EXT && it->second.lastFrame + 600 < m_frame)
        it = m.targets.erase(it);
      else
        ++it;
    }
    std::lock_guard lock(m_mutex);
    m_stats.targets = uint32_t(m.targets.size());
  }
} // namespace remix
