#include "metal/particles.h"
#include "metal/timers.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>

// particles/renderer.cpp on Metal: the pool, frame constants and passes are
// the Vulkan renderer's; descriptor sets are argument buffers of each stage
// (metal::ArgumentSet), barriers are Metal's hazard tracking.
namespace remix::metal {
  namespace {
    // particles/shaders/common.glsl, std430 (particles/renderer.cpp GpuFrame, GpuEmitter).
    struct GpuFrame {
      float viewProjection[16];
      float camera[4], cameraRight[4], cameraUp[4], cameraDelta[4], worldUp[4];
      float sunDirection[4], sunRadiance[4], ambient[4], volumeOrigin[4];
      int32_t volumeCell[4];
      uint32_t volumeSize[4], display[4];
      float depthScale[4];
      float dome[3][4];
      uint32_t counts[4];
      float cloudShadow[3][4];
      uint32_t cloudShadowConfig[4];
    };
    static_assert(sizeof(GpuFrame) == 400);
    struct GpuEmitter {
      float transform[3][4];
      float extent[4];
      uint32_t kind[4], slice[4], groups[4];
      float velocity[4], axis[4], acceleration[4], motion[4], size[4], colorStart[4], colorEnd[4], emission[4];
    };
    static_assert(sizeof(GpuEmitter) == 240);
    constexpr uint32_t particleBytes = 64;
    constexpr uint32_t maxEmitters = scene::maxParticleEmitters;
    constexpr uint32_t group = 64;
    // The light volume: cells of a metre, half as many along the up axis.
    constexpr uint32_t volumeAcross = 32, volumeUp = 16;
    constexpr uint32_t volumeCells = volumeAcross * volumeAcross * volumeUp;
    constexpr size_t frameBytes = sizeof(GpuFrame) + sizeof(GpuEmitter) * maxEmitters;
    constexpr size_t lightBytes = (4 + volumeCells) * sizeof(float);
    constexpr size_t indirectBytes = 16; // MTLDrawPrimitivesIndirectArguments, as VkDrawIndirectCommand
    constexpr size_t argumentAlignment = 256;

    uint32_t groups(uint64_t threads) { return uint32_t((threads + group - 1) / group); }
    size_t align(size_t value) { return (value + argumentAlignment - 1) / argumentAlignment * argumentAlignment; }

    id<MTLBuffer> privateBuffer(id<MTLDevice> device, size_t size) {
      id<MTLBuffer> result = [device newBufferWithLength:std::max<size_t>(size, 16) options:MTLResourceStorageModePrivate];
      check(result, "particle buffer");
      return result;
    }
  } // namespace

  struct Particles::Gpu {
    id<MTLDevice> device = nil;
    bool rayQuery = false;
    // Compute stages: simulate, spawn, args, light (traced), spawn (traced).
    enum Stage { Simulate, Spawn, Args, Light, SpawnTraced, StageCount };
    static constexpr shaders::Stage stages[StageCount] = {
        shaders::Stage::particleSimulate, shaders::Stage::particleSpawn, shaders::Stage::particleArgs,
        shaders::Stage::particleLight, shaders::Stage::particleSpawnTraced};
    struct Compute {
      id<MTLComputePipelineState> pipeline = nil;
      ArgumentSet pool, traced; // sets 0 and 1
    } compute[StageCount];
    id<MTLRenderPipelineState> draw[2] = {}; // additive, premultiplied
    MTLPixelFormat color = MTLPixelFormatInvalid, depthFormat = MTLPixelFormatInvalid;
    id<MTLDepthStencilState> depthState = nil;
    ArgumentSet vertexPool, fragmentPool, fragmentImages; // particleVert set 0, particleFrag sets 0 and 1
    id<MTLSamplerState> sampler = nil, nearest = nil;
    // A white texel: the texture of emitters without one, and the depth
    // binding when the scene's depth is not captured.
    id<MTLTexture> white = nil;

    uint32_t budget = 0;
    id<MTLBuffer> particles[2] = {}, counts[2] = {}, indirect = nil, statistics = nil, light = nil;
    bool countsReady = false;
    uint32_t parity = 0; // the target pool this frame
    struct Slot {
      id<MTLBuffer> frame = nil, readback = nil, images = nil; // images: fragment set 1 per draw
      bool submitted = false;
    } slots[gpuFrameCount];
    uint32_t slot = 0;

    // The frame being drawn.
    std::vector<uint32_t> blends;
    std::vector<id<MTLTexture>> textures; // per emitter; nil: the round sprite
    uint32_t drawn = 0;
    // Resets asked for while the passes could not run.
    std::array<bool, maxEmitters> pendingReset{};
    bool pendingResetAll = true;
    float previousCamera[3] = {};
    bool cameraKnown = false;

    bool preparePipelines(bool traced, MTLPixelFormat target, MTLPixelFormat depth);
    void ensurePool(id<MTLCommandBuffer> command, uint32_t size);
  };

  bool Particles::Gpu::preparePipelines(bool traced, MTLPixelFormat target, MTLPixelFormat depth) {
    bool ready = true;
    for (int stage = 0; stage < StageCount; ++stage) {
      const bool tracedStage = stage == Light || stage == SpawnTraced;
      if (tracedStage && !(traced && rayQuery))
        continue;
      auto &c = compute[stage];
      if (!c.pipeline) {
        // The untraced stages are small and a frame without them loses its
        // spawns (the registry has handed them out): they compile at once.
        c.pipeline = tracedStage ? computePipelineAsync(device, stages[stage], 0) : computePipeline(device, stages[stage]);
        if (c.pipeline) {
          c.pool = ArgumentSet(device, stages[stage], 0);
          if (tracedStage)
            c.traced = ArgumentSet(device, stages[stage], 1);
        }
      }
      // The traced stages are optional: without them the frame runs untraced.
      ready &= tracedStage || c.pipeline != nil;
    }
    if (target != color || depth != depthFormat) {
      draw[0] = draw[1] = nil;
      color = target;
      depthFormat = depth;
    }
    for (uint32_t blend = 0; blend < 2; ++blend) {
      if (draw[blend])
        continue;
      auto *descriptor = [MTLRenderPipelineDescriptor new];
      descriptor.label = blend ? @"Particles (premultiplied)" : @"Particles (additive)";
      descriptor.vertexFunction = function(device, shaders::Stage::particleVert);
      descriptor.fragmentFunction = function(device, shaders::Stage::particleFrag);
      // The fragment shader's colour is premultiplied by its alpha.
      auto *attachment = descriptor.colorAttachments[0];
      attachment.pixelFormat = color;
      attachment.blendingEnabled = YES;
      attachment.sourceRGBBlendFactor = MTLBlendFactorOne;
      attachment.destinationRGBBlendFactor = blend ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
      attachment.sourceAlphaBlendFactor = blend ? MTLBlendFactorOne : MTLBlendFactorZero;
      attachment.destinationAlphaBlendFactor = blend ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOne;
      descriptor.depthAttachmentPixelFormat = depthFormat;
      NSError *error = nil;
      draw[blend] = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
      check(draw[blend], "particle pipeline", error);
    }
    if (!vertexPool) {
      vertexPool = ArgumentSet(device, shaders::Stage::particleVert, 0);
      fragmentPool = ArgumentSet(device, shaders::Stage::particleFrag, 0);
      fragmentImages = ArgumentSet(device, shaders::Stage::particleFrag, 1);
    }
    return ready;
  }

  void Particles::Gpu::ensurePool(id<MTLCommandBuffer> command, uint32_t size) {
    if (!indirect) {
      indirect = privateBuffer(device, indirectBytes * maxEmitters);
      statistics = privateBuffer(device, 16);
      // Every cell sees the sun until traced.
      light = buffer(device, nullptr, lightBytes);
      std::fill_n(static_cast<float *>(light.contents), lightBytes / sizeof(float), 1.f);
      for (auto &c : counts)
        c = privateBuffer(device, sizeof(uint32_t) * maxEmitters);
      for (auto &s : slots) {
        s.frame = buffer(device, nullptr, frameBytes);
        s.readback = buffer(device, nullptr, 16);
        std::memset(s.readback.contents, 0, 16);
        s.images = buffer(device, nullptr, align(fragmentImages.encodedLength()) * maxEmitters);
      }
    }
    if (size != budget || !particles[0]) {
      for (auto &p : particles)
        p = privateBuffer(device, size_t(std::max(size, 1u)) * particleBytes);
      budget = size;
      countsReady = false;
    }
    if (!countsReady) {
      // Empty slices.
      id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Particles: reset");
      for (auto &c : counts)
        [blit fillBuffer:c range:NSMakeRange(0, c.length) value:0];
      [blit endEncoding];
      countsReady = true;
    }
  }

  Particles::Particles(id<MTLDevice> device, bool rayQuery) : m(std::make_unique<Gpu>()) {
    auto &g = *m;
    g.device = device;
    g.rayQuery = rayQuery;
    auto *descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    descriptor.mipFilter = MTLSamplerMipFilterLinear;
    descriptor.sAddressMode = descriptor.tAddressMode = descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.supportArgumentBuffers = YES;
    g.sampler = [device newSamplerStateWithDescriptor:descriptor];
    descriptor.minFilter = descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = MTLSamplerMipFilterNearest;
    g.nearest = [device newSamplerStateWithDescriptor:descriptor];
    check(g.sampler, "particle sampler");
    check(g.nearest, "particle sampler");
    g.white = texture2d(device, MTLPixelFormatRGBA8Unorm, 1, 1, MTLTextureUsageShaderRead, 1, MTLStorageModeShared);
    const uint8_t one[4] = {255, 255, 255, 255};
    [g.white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:one bytesPerRow:4];
    auto *depth = [MTLDepthStencilDescriptor new];
    depth.depthCompareFunction = MTLCompareFunctionLessEqual;
    depth.depthWriteEnabled = NO;
    g.depthState = [device newDepthStencilStateWithDescriptor:depth];
    check(g.depthState, "particle depth state");
  }

  Particles::~Particles() = default;

  void Particles::selectFrame(uint32_t slot) {
    auto &g = *m;
    g.slot = slot % gpuFrameCount;
    auto &s = g.slots[g.slot];
    if (s.submitted && s.readback) {
      const auto *values = static_cast<const uint32_t *>(s.readback.contents);
      m_report.live = values[0];
      m_report.dropped = values[1];
      m_report.sheltered = values[2];
      s.submitted = false;
    }
  }

  void Particles::defer(const scene::ParticleFrame &frame) {
    for (const auto &emitter : frame.emitters)
      if (emitter.reset)
        m->pendingReset[emitter.slot] = true;
    m->pendingResetAll |= frame.resetAll;
    m->drawn = 0;
  }

  bool Particles::simulate(id<MTLCommandBuffer> command, const scene::ParticleFrame &frame, const Inputs &inputs,
                           const TextureLookup &texture, MTLPixelFormat color, MTLPixelFormat depth) {
    auto &g = *m;
    g.drawn = 0;
    if (!frame.enabled || !frame.budget) {
      g.pendingResetAll = true;
      g.cameraKnown = false;
      return false;
    }
    for (const auto &emitter : frame.emitters)
      if (emitter.reset)
        g.pendingReset[emitter.slot] = true;
    g.pendingResetAll |= frame.resetAll;
    const bool traced = inputs.scene != nil && g.rayQuery;
    if (!g.preparePipelines(traced, color, depth))
      return false;
    const bool tracedReady = traced && g.compute[Gpu::Light].pipeline && g.compute[Gpu::SpawnTraced].pipeline;
    g.ensurePool(command, frame.budget);
    if (g.pendingResetAll) {
      id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Particles: reset");
      for (auto &c : g.counts)
        [blit fillBuffer:c range:NSMakeRange(0, c.length) value:0];
      [blit endEncoding];
      g.pendingResetAll = false;
    }
    auto &slot = g.slots[g.slot];

    // The frame's constants (particles/renderer.cpp).
    auto *out = static_cast<GpuFrame *>(slot.frame.contents);
    GpuFrame f{};
    std::memcpy(f.viewProjection, inputs.viewProjection.data(), sizeof(f.viewProjection));
    const auto &v = inputs.view;
    float camera[3];
    for (int c = 0; c < 3; ++c)
      camera[c] = -(v[c * 4 + 0] * v[12] + v[c * 4 + 1] * v[13] + v[c * 4 + 2] * v[14]);
    const float seconds = frame.seconds;
    for (int i = 0; i < 3; ++i) {
      f.camera[i] = camera[i];
      f.cameraRight[i] = v[i * 4 + 0];
      f.cameraUp[i] = v[i * 4 + 1];
      f.cameraDelta[i] = g.cameraKnown ? camera[i] - g.previousCamera[i] : 0;
      g.previousCamera[i] = camera[i];
      f.sunDirection[i] = inputs.sunDirection[i];
      f.sunRadiance[i] = inputs.sunIrradiance[i];
      f.ambient[i] = inputs.ambient[i];
    }
    g.cameraKnown = true;
    f.camera[3] = seconds;
    f.cameraRight[3] = inputs.time;
    const uint32_t up = std::min(inputs.upAxis, 2u);
    f.worldUp[up] = 1;
    f.worldUp[3] = inputs.exposure;
    // The light volume: a metre a cell around the camera, snapped to cells.
    const float cell = 1 / std::max(inputs.metersPerUnit, 1e-6f);
    for (int i = 0; i < 3; ++i) {
      const uint32_t size = uint32_t(i) == up ? volumeUp : volumeAcross;
      f.volumeSize[i] = size;
      f.volumeCell[i] = int32_t(std::floor(camera[i] / cell)) - int32_t(size / 2) + (uint32_t(i) == up ? 2 : 0);
      f.volumeOrigin[i] = float(f.volumeCell[i]) * cell;
    }
    f.volumeOrigin[3] = cell;
    f.volumeCell[3] = int32_t(frame.frame % 4);
    f.sunRadiance[3] = tracedReady ? 1.f : 0.f;
    const bool environment = tracedReady && inputs.environment != nil;
    f.ambient[3] = environment ? 1.f : 0.f;
    std::copy_n(inputs.display, 4, f.display);
    // View depth from the device depth, as raster.vert's sprites.
    const auto &p = inputs.viewProjection;
    const float zRow[3] = {p[2], p[6], p[10]}, wRow[3] = {p[3], p[7], p[11]};
    const float ww = wRow[0] * wRow[0] + wRow[1] * wRow[1] + wRow[2] * wRow[2];
    if (ww > 1e-20f) {
      const float a = (zRow[0] * wRow[0] + zRow[1] * wRow[1] + zRow[2] * wRow[2]) / ww;
      f.depthScale[0] = a;
      f.depthScale[1] = p[14] - a * p[15];
      f.depthScale[2] = 1;
    }
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        f.dome[r][c] = inputs.domeTransform[r][c];
    f.dome[0][3] = inputs.environmentScale;
    const bool clouds = tracedReady && inputs.cloudShadow != nil && inputs.cloudShadowSlices >= 2;
    if (clouds) {
      std::memcpy(f.cloudShadow, inputs.cloudShadowTransform, sizeof(f.cloudShadow));
      f.cloudShadowConfig[0] = 1;
      f.cloudShadowConfig[1] = inputs.cloudShadowSlices;
    }
    const uint32_t emitterCount = uint32_t(std::min<size_t>(frame.emitters.size(), maxEmitters));
    uint32_t simulateGroups = 0, spawns = 0;
    g.blends.assign(emitterCount, 0);
    g.textures.assign(emitterCount, nil);
    auto *emitters = reinterpret_cast<GpuEmitter *>(reinterpret_cast<char *>(out) + sizeof(GpuFrame));
    for (uint32_t i = 0; i < emitterCount; ++i) {
      const auto &e = frame.emitters[i];
      GpuEmitter x{};
      std::memcpy(x.transform, e.transform, sizeof(x.transform));
      std::copy_n(e.extent, 3, x.extent);
      x.kind[0] = e.shape;
      x.kind[1] = e.facing;
      x.kind[2] = e.blend;
      // SHELTER needs the traced spawn pass.
      x.kind[3] = e.flags;
      x.slice[0] = e.offset;
      x.slice[1] = e.capacity;
      x.slice[2] = e.slot;
      x.slice[3] = e.seed * 2654435761u + e.slot;
      x.groups[0] = simulateGroups;
      x.groups[1] = spawns;
      x.groups[2] = e.spawn;
      x.groups[3] = g.pendingReset[e.slot] ? 1u : 0u;
      g.pendingReset[e.slot] = false;
      simulateGroups += groups(e.capacity);
      spawns += e.spawn;
      std::copy_n(e.velocity, 3, x.velocity);
      x.velocity[3] = e.speed;
      std::copy_n(e.axis, 3, x.axis);
      x.axis[3] = e.cosCone;
      std::copy_n(e.acceleration, 3, x.acceleration);
      x.acceleration[3] = e.drag;
      x.motion[0] = e.lifetime[0];
      x.motion[1] = e.lifetime[1];
      x.motion[2] = e.turbulence;
      x.motion[3] = e.stretch;
      std::copy_n(e.size, 3, x.size);
      x.size[3] = e.softDistance;
      std::copy_n(e.colorStart, 4, x.colorStart);
      std::copy_n(e.colorEnd, 4, x.colorEnd);
      std::copy_n(e.emission, 3, x.emission);
      if (e.texture && texture) {
        g.textures[i] = texture(e.texture);
        x.emission[3] = g.textures[i] ? 1.f : 0.f;
      }
      g.blends[i] = e.blend;
      emitters[i] = x;
    }
    f.volumeSize[3] = emitterCount;
    f.counts[0] = uint32_t(frame.frame);
    f.counts[1] = emitterCount;
    f.counts[2] = groups(spawns);
    f.counts[3] = simulateGroups;
    std::memcpy(out, &f, sizeof(f));

    // The pool's buffers, this frame's target the other one.
    const uint32_t targetPool = g.parity, sourcePool = g.parity ^ 1u;
    const auto pool = [&](ArgumentSet &set) {
      set.begin(g.slot);
      const id<MTLBuffer> buffers[8] = {slot.frame,         g.particles[sourcePool], g.particles[targetPool],
                                        g.counts[sourcePool], g.counts[targetPool],    g.indirect,
                                        g.statistics,        g.light};
      for (uint32_t i = 0; i < 8; ++i)
        if (set.has(i))
          set.buffer(i, buffers[i]);
    };

    // The passes.
    {
      id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Particles: clear");
      [blit fillBuffer:g.counts[targetPool] range:NSMakeRange(0, g.counts[targetPool].length) value:0];
      [blit fillBuffer:g.statistics range:NSMakeRange(0, g.statistics.length) value:0];
      [blit endEncoding];
    }
    const uint32_t zeros[4] = {};
    const auto dispatch = [&](id<MTLComputeCommandEncoder> encoder, Gpu::Stage stage, uint32_t count) {
      auto &c = g.compute[stage];
      pool(c.pool);
      [encoder setComputePipelineState:c.pipeline];
      [encoder setBuffer:c.pool.current() offset:c.pool.offset() atIndex:0];
      c.pool.use(encoder);
      if (stage == Gpu::Light || stage == Gpu::SpawnTraced) {
        c.traced.begin(g.slot);
        c.traced.acceleration(0, inputs.scene);
        if (c.traced.has(1))
          c.traced.texture(1, environment ? inputs.environment : g.white,
                           environment && inputs.environmentSampler ? inputs.environmentSampler : g.sampler);
        if (c.traced.has(2))
          c.traced.texture(2, clouds ? inputs.cloudShadow : g.white,
                           clouds && inputs.cloudShadowSampler ? inputs.cloudShadowSampler : g.sampler);
        [encoder setBuffer:c.traced.current() offset:c.traced.offset() atIndex:1];
        c.traced.use(encoder);
        if (!inputs.referenced.empty())
          [encoder useResources:inputs.referenced.data() count:inputs.referenced.size() usage:MTLResourceUsageRead];
      }
      const auto &source = library(Gpu::stages[stage]);
      if (source.push != 0xff)
        [encoder setBytes:zeros length:sizeof(zeros) atIndex:source.push];
      [encoder dispatchThreadgroups:MTLSizeMake(count, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(source.threads[0], source.threads[1], source.threads[2])];
    };
    if (tracedReady) {
      id<MTLComputeCommandEncoder> encoder = computeEncoder(command, @"Particles: light volume");
      dispatch(encoder, Gpu::Light, 1 + groups(volumeCells / 4));
      [encoder endEncoding];
    }
    {
      id<MTLComputeCommandEncoder> encoder = computeEncoder(command, @"Particles: simulation");
      if (simulateGroups)
        dispatch(encoder, Gpu::Simulate, simulateGroups);
      if (spawns)
        dispatch(encoder, tracedReady ? Gpu::SpawnTraced : Gpu::Spawn, groups(spawns));
      dispatch(encoder, Gpu::Args, groups(emitterCount ? emitterCount : 1));
      [encoder endEncoding];
    }
    {
      id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Particles: statistics");
      [blit copyFromBuffer:g.statistics sourceOffset:0 toBuffer:slot.readback destinationOffset:0 size:16];
      [blit endEncoding];
    }
    slot.submitted = true;
    g.parity ^= 1u;
    g.drawn = emitterCount;
    return emitterCount != 0;
  }

  void Particles::draw(id<MTLRenderCommandEncoder> encoder, uint32_t width, uint32_t height, id<MTLTexture> sceneDepth) {
    auto &g = *m;
    if (!g.drawn)
      return;
    auto &slot = g.slots[g.slot];
    auto *frame = static_cast<GpuFrame *>(slot.frame.contents);
    // Soft edges need the captured depth: without it the shader keeps them hard.
    if (!sceneDepth)
      frame->depthScale[2] = 0;
    frame->dome[1][3] = float(width);
    frame->dome[2][3] = float(height);
    [encoder pushDebugGroup:@"Particles: draw"];
    [encoder setViewport:{0, 0, double(width), double(height), 0, 1}];
    [encoder setScissorRect:{0, 0, width, height}];
    [encoder setDepthStencilState:g.depthState];
    [encoder setCullMode:MTLCullModeNone];
    // The pool, as the passes left it (this frame's target is now the source parity's other).
    const uint32_t targetPool = g.parity ^ 1u, sourcePool = g.parity;
    const id<MTLBuffer> buffers[8] = {slot.frame,         g.particles[sourcePool], g.particles[targetPool],
                                      g.counts[sourcePool], g.counts[targetPool],    g.indirect,
                                      g.statistics,        g.light};
    for (auto *set : {&g.vertexPool, &g.fragmentPool}) {
      set->begin(g.slot);
      for (uint32_t i = 0; i < 8; ++i)
        if (set->has(i))
          set->buffer(i, buffers[i]);
    }
    [encoder setVertexBuffer:g.vertexPool.current() offset:g.vertexPool.offset() atIndex:0];
    [encoder setFragmentBuffer:g.fragmentPool.current() offset:g.fragmentPool.offset() atIndex:0];
    g.vertexPool.use(encoder, MTLRenderStageVertex);
    g.fragmentPool.use(encoder, MTLRenderStageFragment);
    [encoder useResource:g.white usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    if (sceneDepth)
      [encoder useResource:sceneDepth usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    // An image set per distinct texture.
    std::map<void *, NSUInteger> sets;
    const NSUInteger stride = align(g.fragmentImages.encodedLength());
    const auto &vertexSource = library(shaders::Stage::particleVert);
    const auto &fragmentSource = library(shaders::Stage::particleFrag);
    uint32_t bound = UINT32_MAX;
    for (uint32_t i = 0; i < g.drawn; ++i) {
      id<MTLTexture> image = g.textures[i] ? g.textures[i] : g.white;
      auto found = sets.find((__bridge void *)image);
      if (found == sets.end()) {
        const NSUInteger offset = stride * sets.size();
        g.fragmentImages.begin(slot.images, offset);
        g.fragmentImages.texture(0, sceneDepth ? sceneDepth : g.white, g.nearest);
        g.fragmentImages.texture(1, image, g.sampler);
        [encoder useResource:image usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
        found = sets.emplace((__bridge void *)image, offset).first;
      }
      [encoder setFragmentBuffer:slot.images offset:found->second atIndex:1];
      const uint32_t blend = std::min(g.blends[i], 1u);
      if (blend != bound) {
        [encoder setRenderPipelineState:g.draw[blend]];
        bound = blend;
      }
      const uint32_t push[4] = {i, 0, 0, 0};
      if (vertexSource.push != 0xff)
        [encoder setVertexBytes:push length:sizeof(push) atIndex:vertexSource.push];
      if (fragmentSource.push != 0xff)
        [encoder setFragmentBytes:push length:sizeof(push) atIndex:fragmentSource.push];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:g.indirect indirectBufferOffset:i * indirectBytes];
    }
    [encoder popDebugGroup];
  }
} // namespace remix::metal
