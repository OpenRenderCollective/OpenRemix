#include "metal/raster_gpu.h"
#include <algorithm>
#include <cmath>

// The raster renderer's overlay passes on Metal: halo outlines
// (raster/halo.cpp), masked overlays and role CUT (raster/mask.cpp) and the
// scene depth of soft sprite overlays (raster/sprite.cpp). Silhouettes are
// drawn as coverage into R8 targets at target resolution, depth-tested
// (biased) against the frame's depth when asked; halos blur their silhouette
// twice and composite it onto the frame outside it.
namespace remix::metal {
  using namespace scene;

  namespace {
    constexpr int blurIterations = 2;
    constexpr int maxTaps = 16;

    struct BlurPush {
      float area[4];
      float texel[2];
      float direction[2];
      float sigma, stride;
      int32_t taps;
      uint32_t padding;
    };
    struct CompositePush {
      float color[4];
      uint32_t flags;
      uint32_t padding[3];
    };
    static_assert(sizeof(BlurPush) == 48 && sizeof(CompositePush) == 32);

    // raster/halo.cpp kernel: one blur pass along an axis.
    constexpr float sigmaPerBlur = 2;
    struct Kernel {
      float sigma, stride;
      int32_t taps;
      float reach; // texels the pass spreads coverage by
    };
    Kernel kernel(float blur, uint32_t height) {
      Kernel k;
      k.sigma = std::max(sigmaPerBlur * blur * float(height) / 1080.f, 1e-3f);
      const float radius = std::max(1.f, std::ceil(2.5f * k.sigma));
      k.taps = std::min(maxTaps, int32_t(std::ceil(radius / 2)));
      k.stride = std::max(1.f, radius / float(2 * k.taps));
      k.reach = float(2 * k.taps) * k.stride;
      return k;
    }

    MTLVertexDescriptor *rasterVertices(uint32_t index) {
      auto *vertex = [MTLVertexDescriptor vertexDescriptor];
      vertex.attributes[0].format = MTLVertexFormatFloat3;
      vertex.attributes[0].offset = offsetof(Vertex, position);
      vertex.attributes[1].format = MTLVertexFormatFloat2;
      vertex.attributes[1].offset = offsetof(Vertex, uv);
      vertex.attributes[2].format = MTLVertexFormatFloat4;
      vertex.attributes[2].offset = offsetof(Vertex, color);
      for (unsigned i = 0; i < 3; ++i)
        vertex.attributes[i].bufferIndex = index;
      vertex.layouts[index].stride = sizeof(Vertex);
      return vertex;
    }

    id<MTLRenderPipelineState> create(id<MTLDevice> device, MTLRenderPipelineDescriptor *descriptor,
                                      const char *what) {
      NSError *error = nil;
      id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
      check(state, what, error);
      return state;
    }

    // A silhouette pass: the color target and the frame's depth, tested
    // (loaded and kept) or not (ahead of the frame: neither).
    MTLRenderPassDescriptor *coveragePass(id<MTLTexture> target, bool clear, id<MTLTexture> depth, bool tested) {
      auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = target;
      pass.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
      pass.depthAttachment.texture = depth;
      pass.depthAttachment.loadAction = tested ? MTLLoadActionLoad : MTLLoadActionDontCare;
      pass.depthAttachment.storeAction = tested ? MTLStoreActionStore : MTLStoreActionDontCare;
      return pass;
    }
  } // namespace

  Raster::Gpu::CoverageTarget Raster::Gpu::coverageTarget(bool forHalo) {
    if (!coverageSampler) {
      auto *descriptor = [MTLSamplerDescriptor new];
      descriptor.minFilter = descriptor.magFilter = MTLSamplerMinMagFilterLinear;
      descriptor.sAddressMode = descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
      descriptor.supportArgumentBuffers = YES;
      coverageSampler = [device newSamplerStateWithDescriptor:descriptor];
      check(coverageSampler, "coverage sampler");
    }
    CoverageTarget target;
    target.texture =
        texture2d(device, coverageFormat, width, height, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    target.texture.label = forHalo ? @"Halo coverage" : @"Mask coverage";
    const auto set = [&](ArgumentSet &out, shaders::Stage stage, uint32_t index) {
      out = ArgumentSet(device, stage, index);
      out.begin(0);
      out.texture(0, target.texture, coverageSampler);
    };
    if (forHalo) {
      set(target.blur, shaders::Stage::haloBlur, 0);
      set(target.composite, shaders::Stage::haloComposite, 0);
      set(target.mask, shaders::Stage::haloComposite, 1);
    } else {
      set(target.overlay, shaders::Stage::maskOverlay, 1);
      set(target.within, shaders::Stage::maskWithin, 1);
    }
    return target;
  }

  id<MTLRenderPipelineState> Raster::Gpu::silhouettePipeline(bool within) {
    if (silhouettes[within])
      return silhouettes[within];
    // The raster vertex stage and the material's alpha test; coverage in red.
    auto *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.label = within ? @"Mask source (within)" : @"Silhouette";
    descriptor.vertexFunction = function(device, shaders::Stage::rasterVert);
    descriptor.fragmentFunction =
        function(device, within ? shaders::Stage::maskWithin : shaders::Stage::haloMask);
    descriptor.vertexDescriptor = rasterVertices(vertexBufferIndex);
    descriptor.colorAttachments[0].pixelFormat = coverageFormat;
    descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskRed;
    descriptor.depthAttachmentPixelFormat = depthFormat;
    silhouettes[within] = create(device, descriptor, "silhouette pipeline");
    return silhouettes[within];
  }

  void Raster::Gpu::prepareSilhouettes() {
    for (int tested = 0; tested < 2; ++tested)
      if (!silhouetteDepth[tested]) {
        auto *descriptor = [MTLDepthStencilDescriptor new];
        descriptor.depthCompareFunction = tested ? MTLCompareFunctionLessEqual : MTLCompareFunctionAlways;
        descriptor.depthWriteEnabled = NO;
        silhouetteDepth[tested] = [device newDepthStencilStateWithDescriptor:descriptor];
        check(silhouetteDepth[tested], "silhouette depth state");
      }
    silhouettePipeline(false);
  }

  void Raster::Gpu::prepareHalo() {
    prepareSilhouettes();
    auto &h = halo;
    if (!h.targets[0].texture)
      for (auto &target : h.targets)
        target = coverageTarget(true);
    const auto fullscreen = [&](bool blur, bool additive) {
      auto *descriptor = [MTLRenderPipelineDescriptor new];
      descriptor.label = blur ? @"Halo blur" : @"Halo composite";
      descriptor.vertexFunction = function(device, shaders::Stage::haloVert);
      descriptor.fragmentFunction =
          function(device, blur ? shaders::Stage::haloBlur : shaders::Stage::haloComposite);
      auto *color = descriptor.colorAttachments[0];
      if (blur) {
        color.pixelFormat = coverageFormat;
        color.writeMask = MTLColorWriteMaskRed;
      } else {
        // Additive, or the reference's "Modulate": the frame times the blurred mask.
        color.pixelFormat = displayFormat;
        color.blendingEnabled = YES;
        color.sourceRGBBlendFactor = additive ? MTLBlendFactorOne : MTLBlendFactorDestinationColor;
        color.destinationRGBBlendFactor = additive ? MTLBlendFactorOne : MTLBlendFactorZero;
        color.sourceAlphaBlendFactor = MTLBlendFactorZero;
        color.destinationAlphaBlendFactor = MTLBlendFactorOne;
        color.writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        descriptor.depthAttachmentPixelFormat = depthFormat;
      }
      return create(device, descriptor, "halo pipeline");
    };
    if (!h.blur)
      h.blur = fullscreen(true, false);
    for (int additive = 0; additive < 2; ++additive)
      if (!h.composite[additive])
        h.composite[additive] = fullscreen(false, additive != 0);
    if (!noDepth) {
      auto *descriptor = [MTLDepthStencilDescriptor new];
      noDepth = [device newDepthStencilStateWithDescriptor:descriptor];
      check(noDepth, "depth state");
    }
  }

  bool Raster::Gpu::coverageArea(Halo::Group &group, const float *margin) const {
    if (!group.count || !group.bounded || !width || !height)
      return false;
    const float size[2] = {float(width), float(height)};
    int32_t low[2], high[2];
    for (int c = 0; c < 2; ++c) {
      const float a = (group.low[c] * .5f + .5f) * size[c] - margin[c];
      const float b = (group.high[c] * .5f + .5f) * size[c] + margin[c];
      low[c] = int32_t(std::floor(std::clamp(a, 0.f, size[c])));
      high[c] = int32_t(std::ceil(std::clamp(b, 0.f, size[c])));
      if (high[c] <= low[c])
        return false;
    }
    group.area = {NSUInteger(low[0]), NSUInteger(low[1]), NSUInteger(high[0] - low[0]), NSUInteger(high[1] - low[1])};
    return true;
  }

  bool Raster::Gpu::haloArea(Halo::Group &group) const {
    const float blur[2] = {group.halo.blurX, group.halo.blurY};
    float margin[2];
    // Each iteration spreads the silhouette by the kernel's reach.
    for (int c = 0; c < 2; ++c)
      margin[c] = blurIterations * kernel(blur[c], height).reach + 2;
    return coverageArea(group, margin);
  }

  void Raster::Gpu::drawSilhouettes(id<MTLRenderCommandEncoder> encoder, const std::vector<Command> &list,
                                    uint32_t first, uint32_t count, int32_t within, bool early) {
    [encoder setViewport:{0, 0, double(width), double(height), 0, 1}];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setRenderPipelineState:silhouettePipeline(within >= 0)];
    if (within >= 0) {
      const auto &limit = mask.targets[size_t(within)];
      [encoder setFragmentBuffer:limit.within.current() offset:0 atIndex:1];
      [encoder useResource:limit.texture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    }
    const auto fragment = within >= 0 ? shaders::Stage::maskWithin : shaders::Stage::haloMask;
    int tested = -1;
    for (uint32_t i = first; i < first + count; ++i) {
      const auto &draw = list[i];
      // Ahead of the scene nothing is depth-tested.
      const bool test = draw.tested && !early;
      if (int(test) != tested) {
        // The frame's depth comes from another pipeline (the tracer's primary
        // surface): the test is biased toward the camera by a few depth steps
        // and by the surface's slope over a pixel.
        [encoder setDepthStencilState:silhouetteDepth[test]];
        if (test)
          [encoder setDepthBias:-16 slopeScale:-2 clamp:0];
        else
          [encoder setDepthBias:0 slopeScale:0 clamp:0];
        tested = test;
      }
      [encoder setVertexBuffer:draw.vertices offset:draw.vertexOffset atIndex:vertexBufferIndex];
      [encoder setVertexBytes:&draw.push length:sizeof(draw.push) atIndex:library(shaders::Stage::rasterVert).push];
      [encoder setFragmentBuffer:draw.material offset:0 atIndex:0];
      [encoder useResource:draw.texture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
      [encoder setFragmentBytes:&draw.push length:sizeof(draw.push) atIndex:library(fragment).push];
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:draw.count
                           indexType:MTLIndexTypeUInt32
                         indexBuffer:draw.indices
                   indexBufferOffset:draw.indexOffset + size_t(draw.firstIndex) * sizeof(uint32_t)
                       instanceCount:1
                          baseVertex:draw.baseVertex
                        baseInstance:0];
    }
  }

  void Raster::Gpu::recordMasks(id<MTLCommandBuffer> command, bool early) {
    for (const auto &run : mask.runs) {
      if (early && !run.early)
        continue;
      // The whole target is cleared: masked draws sample it anywhere.
      auto *pass = coveragePass(mask.targets[run.target].texture, run.clear, depth, !early);
      id<MTLRenderCommandEncoder> encoder =
          renderEncoder(command, pass, early ? @"Mask coverage (cut)" : @"Mask coverage", width, height);
      [encoder setFrontFacingWinding:MTLWindingClockwise];
      drawSilhouettes(encoder, mask.commands, run.first, run.count, run.within, early);
      [encoder endEncoding];
    }
  }

  void Raster::Gpu::recordHaloMask(id<MTLCommandBuffer> command, uint32_t index) {
    auto &h = halo;
    const auto &group = h.groups[index];
    {
      id<MTLRenderCommandEncoder> encoder =
          renderEncoder(command, coveragePass(h.targets[0].texture, true, depth, true), @"Halo mask", width, height);
      [encoder setFrontFacingWinding:MTLWindingClockwise];
      [encoder setScissorRect:group.area];
      drawSilhouettes(encoder, h.commands, group.first, group.count, -1, false);
      [encoder endEncoding];
    }
    // Horizontal then vertical, twice: mask -> A -> B -> A -> B.
    const Kernel kernels[2] = {kernel(group.halo.blurX, height), kernel(group.halo.blurY, height)};
    int source = 0;
    for (int pass = 0; pass < 2 * blurIterations; ++pass) {
      const int axis = pass & 1;
      const int target = axis ? 2 : 1;
      const auto &k = kernels[axis];
      BlurPush push = {};
      push.area[0] = float(group.area.x) + .5f;
      push.area[1] = float(group.area.y) + .5f;
      push.area[2] = float(group.area.x + group.area.width) - .5f;
      push.area[3] = float(group.area.y + group.area.height) - .5f;
      push.texel[0] = 1.f / float(width);
      push.texel[1] = 1.f / float(height);
      push.direction[axis] = 1;
      push.sigma = k.sigma;
      push.stride = k.stride;
      push.taps = k.taps;
      // Every pixel of the area is written.
      auto *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
      descriptor.colorAttachments[0].texture = h.targets[target].texture;
      descriptor.colorAttachments[0].loadAction = MTLLoadActionDontCare;
      descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> encoder = renderEncoder(command, descriptor, @"Halo blur", width, height);
      [encoder setViewport:{0, 0, double(width), double(height), 0, 1}];
      [encoder setScissorRect:group.area];
      [encoder setRenderPipelineState:h.blur];
      [encoder setFragmentBuffer:h.targets[source].blur.current() offset:0 atIndex:0];
      [encoder useResource:h.targets[source].texture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
      [encoder setFragmentBytes:&push length:sizeof(push) atIndex:library(shaders::Stage::haloBlur).push];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [encoder endEncoding];
      source = target;
    }
  }

  void Raster::Gpu::recordHaloComposite(id<MTLRenderCommandEncoder> encoder, uint32_t index) {
    auto &h = halo;
    const auto &group = h.groups[index];
    [encoder setViewport:{0, 0, double(width), double(height), 0, 1}];
    [encoder setScissorRect:group.area];
    [encoder setRenderPipelineState:h.composite[group.halo.additive]];
    [encoder setDepthStencilState:noDepth];
    [encoder setCullMode:MTLCullModeNone];
    // The blurred coverage (the last blur target) and the silhouette mask.
    [encoder setFragmentBuffer:h.targets[2].composite.current() offset:0 atIndex:0];
    [encoder setFragmentBuffer:h.targets[0].mask.current() offset:0 atIndex:1];
    [encoder useResource:h.targets[2].texture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [encoder useResource:h.targets[0].texture usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    CompositePush push = {};
    std::copy_n(group.halo.color, 3, push.color);
    push.color[3] = float(group.halo.passes);
    push.flags = uint32_t(group.halo.additive) | (linearTarget ? 2u : 0u);
    [encoder setFragmentBytes:&push length:sizeof(push) atIndex:library(shaders::Stage::haloComposite).push];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder setScissorRect:{0, 0, width, height}];
  }

  bool Raster::Gpu::testsSceneDepth() const {
    if (sprite.soft)
      return true;
    for (const auto &command : commands)
      if (command.layer == 1)
        return true;
    for (const auto *list : {&mask.commands, &halo.commands})
      for (const auto &command : *list)
        if (command.tested)
          return true;
    return false;
  }

  void Raster::Gpu::captureSceneDepth(id<MTLCommandBuffer> command) {
    if (!sprite.soft || !depth)
      return;
    if (!sprite.depth) {
      // Sampled as a texture: its red is the depth.
      sprite.depth = texture2d(device, depthFormat, width, height, MTLTextureUsageShaderRead);
      sprite.depth.label = @"Scene depth for soft overlays";
      auto *descriptor = [MTLSamplerDescriptor new];
      descriptor.sAddressMode = descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
      descriptor.supportArgumentBuffers = YES;
      id<MTLSamplerState> nearest = [device newSamplerStateWithDescriptor:descriptor];
      check(nearest, "depth sampler");
      sprite.set = ArgumentSet(device, shaders::Stage::spriteFrag, 1);
      sprite.set.begin(0);
      sprite.set.texture(0, sprite.depth, nearest);
    }
    id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Scene depth for soft overlays");
    [blit copyFromTexture:depth toTexture:sprite.depth];
    [blit endEncoding];
    sprite.captured = true;
  }
} // namespace remix::metal
