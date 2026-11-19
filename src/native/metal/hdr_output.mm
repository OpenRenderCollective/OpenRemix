#include "metal/runtime_gpu.h"
#include "runtime/hdr_nits.h"

// HDR10 output on Metal (runtime/hdr_output.h): while the display has extended
// dynamic range, the layer is BT.2100 PQ (10 bits) and the frame's passes draw
// into a float back buffer, gamma 2.2 encoded with 1.0 at paper white; the
// encode pass converts it to BT.2020 PQ into the drawable. The HUD layer and
// the developer UI are drawn at their own brightness between scaleHdrUi calls.
namespace remix {
  namespace {
    constexpr MTLPixelFormat sdrFormat = MTLPixelFormatBGRA8Unorm, backBufferFormat = MTLPixelFormatRGBA16Float,
                             pqFormat = MTLPixelFormatBGR10A2Unorm;
  } // namespace

  void RemixNativeRuntime::configureHdr() {
    auto &g = *m_gpu;
    auto &h = g.hdr;
    const auto &settings = m_profiles.current();
    const bool wanted = settings.hdr && m_window.hdrEnabled() && !settings.frameGeneration;
    h.status = wanted                   ? std::string()
               : !settings.hdr          ? "turned off"
               : !m_window.hdrEnabled() ? "the display has no extended dynamic range"
                                        : "frame generation is on (Metal presents it in SDR)";
    if (wanted != h.active) {
      // The frames in flight finish with the formats they were recorded for.
      id<MTLCommandBuffer> drain = [g.queue commandBuffer];
      [drain commit];
      [drain waitUntilCompleted];
      h.active = wanted;
      g.format = wanted ? backBufferFormat : sdrFormat;
      g.layer.pixelFormat = wanted ? pqFormat : sdrFormat;
      g.layer.wantsExtendedDynamicRangeContent = wanted;
      if (wanted) {
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ);
        g.layer.colorspace = space;
        CGColorSpaceRelease(space);
      } else
        g.layer.colorspace = nil;
      for (auto &buffer : h.backBuffers)
        buffer = nil;
    }
    if (!h.active)
      return;
    for (auto &buffer : h.backBuffers)
      if (!buffer || buffer.width != g.width || buffer.height != g.height) {
        buffer = metal::texture2d(g.device, backBufferFormat, g.width, g.height,
                                  MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        buffer.label = @"HDR10 back buffer";
      }
    if (!h.encode) {
      const auto pipeline = [&](bool encode) {
        auto *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.label = encode ? @"HDR10 encode" : @"HDR10 UI brightness";
        descriptor.vertexFunction = metal::function(g.device, metal::shaders::Stage::haloVert);
        descriptor.fragmentFunction =
            metal::function(g.device, encode ? metal::shaders::Stage::hdr10 : metal::shaders::Stage::hdrUi);
        auto *color = descriptor.colorAttachments[0];
        color.pixelFormat = encode ? pqFormat : backBufferFormat;
        if (!encode) {
          // The back buffer times the blend constant.
          color.blendingEnabled = YES;
          color.sourceRGBBlendFactor = MTLBlendFactorZero;
          color.destinationRGBBlendFactor = MTLBlendFactorBlendColor;
          color.sourceAlphaBlendFactor = MTLBlendFactorZero;
          color.destinationAlphaBlendFactor = MTLBlendFactorOne;
          descriptor.depthAttachmentPixelFormat = metal::Raster::Gpu::depthFormat;
        }
        NSError *error = nil;
        id<MTLRenderPipelineState> result = [g.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        metal::check(result, "HDR10 pipeline", error);
        return result;
      };
      h.encode = pipeline(true);
      h.scale = pipeline(false);
      h.set = metal::ArgumentSet(g.device, metal::shaders::Stage::hdr10, 0);
      auto *descriptor = [MTLDepthStencilDescriptor new];
      h.noDepth = [g.device newDepthStencilStateWithDescriptor:descriptor];
      auto *sampler = [MTLSamplerDescriptor new];
      sampler.supportArgumentBuffers = YES;
      h.sampler = [g.device newSamplerStateWithDescriptor:sampler];
    }
  }

  scene::DisplayOutput RemixNativeRuntime::displayOutput() const {
    const auto &settings = frameSettings();
    scene::DisplayOutput display;
    display.prism = settings.trace.prism;
    if (!m_gpu || !m_gpu->hdr.active)
      return display;
    const auto nits = HdrNits{settings.hdrPeakNits, settings.hdrPaperWhiteNits, settings.hdrUiNits}.clamped();
    display.hdr10 = true;
    display.peakNits = nits.peak;
    display.paperWhiteNits = nits.paperWhite;
    return display;
  }

  void RemixNativeRuntime::scaleHdrUi(void *renderEncoder, bool before) {
    auto &g = *m_gpu;
    if (!g.hdr.active)
      return;
    const auto &settings = frameSettings();
    const float scale = HdrNits{settings.hdrPeakNits, settings.hdrPaperWhiteNits, settings.hdrUiNits}.uiScale();
    const float factor = before ? 1 / scale : scale;
    id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
    [encoder setViewport:{0, 0, double(g.width), double(g.height), 0, 1}];
    [encoder setScissorRect:{0, 0, g.width, g.height}];
    [encoder setRenderPipelineState:g.hdr.scale];
    [encoder setDepthStencilState:g.hdr.noDepth];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setBlendColorRed:factor green:factor blue:factor alpha:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }

  void RemixNativeRuntime::encodeHdr(void *commandBuffer, void *drawableTexture) {
    auto &g = *m_gpu;
    auto &h = g.hdr;
    id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)commandBuffer;
    auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)drawableTexture;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder =
        metal::renderEncoder(command, pass, @"HDR10 encode (BT.2020 PQ)", g.width, g.height);
    id<MTLTexture> backBuffer = h.backBuffers[g.frameIndex];
    h.set.begin(g.frameIndex);
    h.set.texture(0, backBuffer, h.sampler);
    [encoder setRenderPipelineState:h.encode];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setFragmentBuffer:h.set.current() offset:h.set.offset() atIndex:0];
    [encoder useResource:backBuffer usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    const auto &settings = frameSettings();
    const float paperWhite =
        HdrNits{settings.hdrPeakNits, settings.hdrPaperWhiteNits, settings.hdrUiNits}.clamped().paperWhite;
    const auto &source = metal::library(metal::shaders::Stage::hdr10);
    if (source.push != 0xff)
      [encoder setFragmentBytes:&paperWhite length:sizeof(paperWhite) atIndex:source.push];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
  }
} // namespace remix
