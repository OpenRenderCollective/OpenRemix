// MetalFX reconstruction: upscale/renderer.cpp's input preparation and
// composite around MetalFX's temporal scaler (FSR 3.1 / DLSS-SR's place) and
// temporal denoised scaler (DLSS Ray Reconstruction's place).
#include "metal/reconstruction.h"
#include "scene/display.h"
#include "upscale/camera.h"
#import <MetalFX/MetalFX.h>
#include <algorithm>
#include <cmath>
#include <simd/simd.h>

namespace remix::metal::reconstruction {
  using shaders::Stage;
  namespace {
    // FSR 3.1's quality ratios: native AA, quality, balanced, performance,
    // ultra performance.
    constexpr float ratios[] = {1.f, 1.5f, 1.7f, 2.f, 3.f};
    // The roughness the tracer packs in the RR normal guide's alpha, as the
    // denoised scaler's separate roughness input.
    const char *roughnessSource = R"(
#include <metal_stdlib>
using namespace metal;
kernel void extractRoughness(texture2d<float, access::read> normalRoughness [[texture(0)]],
                             texture2d<float, access::write> roughness [[texture(1)]],
                             uint2 p [[thread_position_in_grid]]) {
  if (p.x >= roughness.get_width() || p.y >= roughness.get_height())
    return;
  roughness.write(float4(normalRoughness.read(p).w), p);
}
)";
    simd_float4x4 matrix(const scene::Matrix &m) {
      simd_float4x4 result;
      for (int c = 0; c < 4; ++c)
        result.columns[c] = simd_make_float4(m[c * 4], m[c * 4 + 1], m[c * 4 + 2], m[c * 4 + 3]);
      return result;
    }
  } // namespace

  const char *name(Backend backend) {
    switch (backend) {
    case Backend::MetalFXUpscaling:
      return "MetalFX upscaling";
    case Backend::MetalFXDenoising:
      return "MetalFX denoising";
    default:
      return "Native";
    }
  }

  bool upscalingSupported(id<MTLDevice> device, std::string &reason) {
    if (![MTLFXTemporalScalerDescriptor supportsDevice:device]) {
      reason = "MetalFX temporal upscaling is not supported on this device";
      return false;
    }
    reason.clear();
    return true;
  }

  bool denoisingSupported(id<MTLDevice> device, std::string &reason) {
    if (@available(macOS 26.0, *)) {
      if ([MTLFXTemporalDenoisedScalerDescriptor supportsDevice:device]) {
        reason.clear();
        return true;
      }
      reason = "MetalFX denoising is not supported on this device";
      return false;
    }
    reason = "MetalFX denoising requires macOS 26";
    return false;
  }

  void renderSize(id<MTLDevice> device, Backend backend, uint32_t quality, uint32_t displayWidth,
                  uint32_t displayHeight, uint32_t &width, uint32_t &height) {
    float ratio = ratios[std::min(quality, 4u)];
    float minimum = 1, maximum = 3;
    if (backend == Backend::MetalFXDenoising) {
      if (@available(macOS 26.0, *)) {
        minimum = [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMinScaleForDevice:device];
        maximum = [MTLFXTemporalDenoisedScalerDescriptor supportedInputContentMaxScaleForDevice:device];
      }
    } else if (@available(macOS 14.0, *)) {
      minimum = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:device];
      maximum = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:device];
    }
    ratio = std::clamp(ratio, std::max(minimum, 1.f), std::max(maximum, 1.f));
    width = std::max(1u, uint32_t(std::lround(displayWidth / ratio)));
    height = std::max(1u, uint32_t(std::lround(displayHeight / ratio)));
  }

  bool supportsCamera(const scene::Matrix &projection) {
    if (std::abs(projection[11]) < .0001f || std::abs(projection[15]) > .0001f || !projection[5])
      return false;
    const auto [nearPlane, farPlane] = upscale::depthRange(projection);
    return std::isfinite(nearPlane) && std::isfinite(farPlane) && nearPlane > 0 && farPlane > nearPlane;
  }

  void jitter(float *xy, uint64_t frame, uint32_t renderWidth, uint32_t displayWidth) {
    const float scale = float(displayWidth) / float(std::max(renderWidth, 1u));
    const uint32_t phases = std::max(1u, uint32_t(std::ceil(8 * scale * scale)));
    auto halton = [](uint32_t n, uint32_t base) {
      float value = 0, f = 1.f / base;
      for (; n; n /= base, f /= base)
        value += float(n % base) * f;
      return value;
    };
    const uint32_t index = uint32_t(frame % phases) + 1;
    xy[0] = halton(index, 2) - .5f;
    xy[1] = halton(index, 3) - .5f;
  }

  struct Renderer::Gpu {
    id<MTLDevice> device = nil;
    Statistics stats;
    id<MTLTexture> deviceDepth = nil, motion = nil, reactive = nil, composition = nil, output = nil, roughness = nil;
    id<MTLComputePipelineState> prepare = nil, extract = nil;
    ArgumentSet prepareSet, compositeSet, postSet;
    id<MTLRenderPipelineState> compositePipeline = nil;
    id<MTLDepthStencilState> compositeDepth = nil;
    MTLPixelFormat displayFormat = MTLPixelFormatInvalid;
    id<MTLFXTemporalScaler> scaler = nil;
    id denoiser = nil; // id<MTLFXTemporalDenoisedScaler> (macOS 26)
    bool initialized = false;
    uint32_t slot = 0;
  };

  Renderer::Renderer() : m_gpu(std::make_unique<Gpu>()) {}
  Renderer::~Renderer() = default;

  void Renderer::initialize(id<MTLDevice> device, Backend backend, uint32_t renderWidth, uint32_t renderHeight,
                            uint32_t displayWidth, uint32_t displayHeight, MTLPixelFormat displayFormat) {
    auto &g = *m_gpu;
    g.device = device;
    g.displayFormat = displayFormat;
    g.stats = {backend, renderWidth, renderHeight, displayWidth, displayHeight};
    // upscale/renderer.cpp's intermediate images: device depth, reactive and
    // composition masks, pixel motion, and the display-size output.
    const auto storage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    g.deviceDepth = texture2d(device, MTLPixelFormatR32Float, renderWidth, renderHeight, storage);
    g.motion = texture2d(device, MTLPixelFormatRG32Float, renderWidth, renderHeight, storage);
    g.reactive = texture2d(device, MTLPixelFormatR8Unorm, renderWidth, renderHeight, storage);
    g.composition = texture2d(device, MTLPixelFormatR8Unorm, renderWidth, renderHeight, storage);
    g.output = texture2d(device, MTLPixelFormatRGBA16Float, displayWidth, displayHeight,
                         storage | MTLTextureUsageRenderTarget);
    for (id<MTLTexture> t : {g.deviceDepth, g.motion, g.reactive, g.composition, g.output})
      g.stats.memoryBytes += t.allocatedSize;
    g.prepare = computePipeline(device, Stage::reconstructPrepare);
    g.prepareSet = ArgumentSet(device, Stage::reconstructPrepare, 0);
    g.compositeSet = ArgumentSet(device, Stage::reconstructFrag, 0);
    g.postSet = ArgumentSet(device, Stage::reconstructFrag, 1);
    if (backend == Backend::MetalFXDenoising) {
      if (@available(macOS 26.0, *)) {
        auto *descriptor = [MTLFXTemporalDenoisedScalerDescriptor new];
        descriptor.colorTextureFormat = MTLPixelFormatRGBA16Float;
        descriptor.depthTextureFormat = MTLPixelFormatR32Float;
        descriptor.motionTextureFormat = MTLPixelFormatRG32Float;
        descriptor.diffuseAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
        descriptor.specularAlbedoTextureFormat = MTLPixelFormatRGBA16Float;
        descriptor.normalTextureFormat = MTLPixelFormatRGBA16Float;
        descriptor.roughnessTextureFormat = MTLPixelFormatR16Float;
        descriptor.specularHitDistanceTextureFormat = MTLPixelFormatR32Float;
        descriptor.specularHitDistanceTextureEnabled = YES;
        descriptor.outputTextureFormat = MTLPixelFormatRGBA16Float;
        descriptor.inputWidth = renderWidth;
        descriptor.inputHeight = renderHeight;
        descriptor.outputWidth = displayWidth;
        descriptor.outputHeight = displayHeight;
        id<MTLFXTemporalDenoisedScaler> denoiser = [descriptor newTemporalDenoisedScalerWithDevice:device];
        check(denoiser, "MetalFX temporal denoised scaler");
        g.denoiser = denoiser;
        g.roughness = texture2d(device, MTLPixelFormatR16Float, renderWidth, renderHeight, storage);
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:@(roughnessSource) options:nil error:&error];
        check(library, "roughness extraction", error);
        g.extract = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"extractRoughness"]
                                                          error:&error];
        check(g.extract, "roughness extraction", error);
      } else
        throw Error("MetalFX denoising requires macOS 26");
    } else if (backend == Backend::MetalFXUpscaling) {
      auto *descriptor = [MTLFXTemporalScalerDescriptor new];
      descriptor.colorTextureFormat = MTLPixelFormatRGBA16Float;
      descriptor.depthTextureFormat = MTLPixelFormatR32Float;
      descriptor.motionTextureFormat = MTLPixelFormatRG32Float;
      descriptor.outputTextureFormat = MTLPixelFormatRGBA16Float;
      descriptor.inputWidth = renderWidth;
      descriptor.inputHeight = renderHeight;
      descriptor.outputWidth = displayWidth;
      descriptor.outputHeight = displayHeight;
      // Exposure is applied by post-processing after reconstruction.
      descriptor.autoExposureEnabled = NO;
      if (@available(macOS 14.4, *)) {
        descriptor.reactiveMaskTextureEnabled = YES;
        descriptor.reactiveMaskTextureFormat = MTLPixelFormatR8Unorm;
      }
      g.scaler = [descriptor newTemporalScalerWithDevice:device];
      check(g.scaler, "MetalFX temporal scaler");
    }
  }

  void Renderer::record(id<MTLCommandBuffer> command, id<MTLTexture> color, id<MTLTexture> viewDepth,
                        id<MTLTexture> motion, id<MTLTexture> transmission, const scene::Matrix &projection,
                        const scene::Matrix &view, const float *jitterOffset, bool reset, const Guides &guides) {
    auto &g = *m_gpu;
    g.slot = (g.slot + 1) % gpuFrameCount;
    // upscale.comp mode 2: reversed device depth (background 0), pixel
    // motion, and reactive/composition masks from transmission.
    {
      g.prepareSet.begin(g.slot);
      const id<MTLTexture> images[] = {viewDepth, transmission, g.deviceDepth, g.reactive, g.composition, motion,
                                       g.motion,  color,        g.output};
      for (uint32_t i = 0; i < 9; ++i)
        g.prepareSet.texture(i, images[i]);
      struct {
        scene::Matrix projection;
        uint32_t mode, padding[3];
      } push{upscale::reversedDepth(projection), 2, {}};
      id<MTLComputeCommandEncoder> encoder = computeEncoder(command, @"Reconstruction input preparation",
                                                            g.stats.renderWidth, g.stats.renderHeight);
      [encoder setComputePipelineState:g.prepare];
      [encoder setBuffer:g.prepareSet.current() offset:0 atIndex:0];
      g.prepareSet.use(encoder);
      [encoder setBytes:&push length:sizeof(push) atIndex:library(Stage::reconstructPrepare).push];
      [encoder dispatchThreadgroups:MTLSizeMake((g.stats.renderWidth + 7) / 8, (g.stats.renderHeight + 7) / 8, 1)
              threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
      if (g.extract) {
        [encoder setComputePipelineState:g.extract];
        [encoder setTexture:guides.normalRoughness atIndex:0];
        [encoder setTexture:g.roughness atIndex:1];
        [encoder dispatchThreadgroups:MTLSizeMake((g.stats.renderWidth + 7) / 8, (g.stats.renderHeight + 7) / 8, 1)
                threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
      }
      [encoder endEncoding];
    }
    const bool resetHistory = reset || !g.initialized;
    // Rays sample pixel + jitter (upscale/renderer.cpp passes FSR -jitter):
    // MetalFX takes the same convention.
    const float jitterX = -jitterOffset[0], jitterY = -jitterOffset[1];
    if (g.scaler) {
      g.scaler.colorTexture = color;
      g.scaler.depthTexture = g.deviceDepth;
      g.scaler.motionTexture = g.motion;
      g.scaler.outputTexture = g.output;
      if (@available(macOS 14.4, *))
        g.scaler.reactiveMaskTexture = g.reactive;
      g.scaler.inputContentWidth = g.stats.renderWidth;
      g.scaler.inputContentHeight = g.stats.renderHeight;
      g.scaler.jitterOffsetX = jitterX;
      g.scaler.jitterOffsetY = jitterY;
      // Motion is in render pixels (pathtrace.comp guideMotion).
      g.scaler.motionVectorScaleX = 1;
      g.scaler.motionVectorScaleY = 1;
      g.scaler.depthReversed = YES;
      g.scaler.preExposure = 1;
      g.scaler.reset = resetHistory;
      [g.scaler encodeToCommandBuffer:command];
      markTimed(command, @"MetalFX temporal scaler");
    } else if (g.denoiser) {
      if (@available(macOS 26.0, *)) {
        id<MTLFXTemporalDenoisedScaler> denoiser = g.denoiser;
        denoiser.colorTexture = color;
        denoiser.depthTexture = g.deviceDepth;
        denoiser.motionTexture = g.motion;
        denoiser.diffuseAlbedoTexture = guides.diffuse;
        denoiser.specularAlbedoTexture = guides.specular;
        denoiser.normalTexture = guides.normalRoughness;
        denoiser.roughnessTexture = g.roughness;
        denoiser.specularHitDistanceTexture = guides.hitDistance;
        denoiser.outputTexture = g.output;
        denoiser.jitterOffsetX = jitterX;
        denoiser.jitterOffsetY = jitterY;
        denoiser.motionVectorScaleX = 1;
        denoiser.motionVectorScaleY = 1;
        denoiser.depthReversed = YES;
        denoiser.preExposure = 1;
        denoiser.shouldResetHistory = resetHistory;
        // Metal clip space: the projection without Vulkan's framebuffer Y flip
        // (as denoise/nrd.cpp hands it to NRD).
        auto clip = projection;
        for (int column = 0; column < 4; ++column)
          clip[column * 4 + 1] = -clip[column * 4 + 1];
        denoiser.worldToViewMatrix = matrix(view);
        denoiser.viewToClipMatrix = matrix(clip);
        [denoiser encodeToCommandBuffer:command];
        markTimed(command, @"MetalFX denoised scaler");
      }
    }
    ++g.stats.frames;
    if (resetHistory)
      ++g.stats.resets;
    g.initialized = true;
  }

  // upscale/renderer.cpp composite.
  void Renderer::composite(id<MTLRenderCommandEncoder> encoder, id<MTLTexture> stableDepth, id<MTLBuffer> exposure,
                           id<MTLTexture> bloom, id<MTLSamplerState> bloomSampler, id<MTLBuffer> postSettings, uint32_t slot) {
    auto &g = *m_gpu;
    if (!g.compositePipeline) {
      auto *descriptor = [MTLRenderPipelineDescriptor new];
      descriptor.label = @"Reconstruction composite";
      descriptor.vertexFunction = function(g.device, Stage::reconstructVert);
      descriptor.fragmentFunction = function(g.device, Stage::reconstructFrag);
      descriptor.colorAttachments[0].pixelFormat = g.displayFormat;
      descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
      NSError *error = nil;
      g.compositePipeline = [g.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
      check(g.compositePipeline, "reconstruction composite", error);
      auto *depth = [MTLDepthStencilDescriptor new];
      depth.depthCompareFunction = MTLCompareFunctionAlways;
      depth.depthWriteEnabled = YES;
      g.compositeDepth = [g.device newDepthStencilStateWithDescriptor:depth];
    }
    g.compositeSet.begin(slot);
    g.compositeSet.texture(0, g.output);
    g.compositeSet.texture(1, stableDepth);
    g.postSet.begin(slot);
    g.postSet.buffer(0, exposure);
    g.postSet.texture(1, bloom, bloomSampler);
    g.postSet.buffer(2, postSettings);
    [encoder setViewport:{0, 0, double(g.stats.displayWidth), double(g.stats.displayHeight), 0, 1}];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setRenderPipelineState:g.compositePipeline];
    [encoder setDepthStencilState:g.compositeDepth];
    [encoder setFragmentBuffer:g.compositeSet.current() offset:0 atIndex:0];
    [encoder setFragmentBuffer:g.postSet.current() offset:0 atIndex:1];
    g.compositeSet.use(encoder, MTLRenderStageFragment);
    g.postSet.use(encoder, MTLRenderStageFragment);
    // upscale.frag's constants: SDR.
    const scene::DisplayOutput display;
    const struct {
      uint32_t display;
      float peakNits, paperWhiteNits;
    } settings{uint32_t(display.hdr10), display.peakNits, display.paperWhiteNits};
    [encoder setFragmentBytes:&settings length:sizeof(settings) atIndex:library(Stage::reconstructFrag).push];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }

  id<MTLTexture> Renderer::output() const { return m_gpu->output; }
  id<MTLTexture> Renderer::deviceDepth() const { return m_gpu->deviceDepth; }
  id<MTLTexture> Renderer::motion() const { return m_gpu->motion; }
  id Renderer::scaler() const { return m_gpu->scaler; }
  Statistics Renderer::statistics() const { return m_gpu->stats; }
} // namespace remix::metal::reconstruction
