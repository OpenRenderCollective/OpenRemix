// The Metal denoiser: denoise/renderer.cpp and denoise/nrd.cpp on Metal. The
// NRD library schedules its dispatches; their shaders are NRD's SPIR-V
// translated with the other stages (scripts/shaders/metal.py).
#include "metal/denoise.h"
#include "denoise/pipeline_selection.h"
#include <NRD.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace remix::metal::denoise {
  using shaders::Stage;
  namespace {
    void require(nrd::Result r) {
      if (r != nrd::Result::SUCCESS)
        throw Error("NRD API rejected denoiser configuration");
    }
    MTLPixelFormat format(nrd::Format value) {
      switch (value) {
      case nrd::Format::R8_UNORM:
        return MTLPixelFormatR8Unorm;
      case nrd::Format::R8_UINT:
        return MTLPixelFormatR8Uint;
      case nrd::Format::R16_UINT:
        return MTLPixelFormatR16Uint;
      case nrd::Format::R16_SFLOAT:
        return MTLPixelFormatR16Float;
      case nrd::Format::R32_UINT:
        return MTLPixelFormatR32Uint;
      case nrd::Format::RG8_UNORM:
        return MTLPixelFormatRG8Unorm;
      case nrd::Format::RGBA8_UNORM:
        return MTLPixelFormatRGBA8Unorm;
      case nrd::Format::R32_SFLOAT:
        return MTLPixelFormatR32Float;
      case nrd::Format::RGBA16_SFLOAT:
        return MTLPixelFormatRGBA16Float;
      case nrd::Format::R10_G10_B10_A2_UNORM:
        return MTLPixelFormatRGB10A2Unorm;
      default:
        throw Error("Unexpected NRD texture format");
      }
    }
    constexpr uint32_t inputUniformBinding = BaseImageCount + 3;
  } // namespace

  // denoise/gpu.h NrdPass: each signal owns its dispatch arguments, constants
  // and temporal history.
  struct NrdPass {
    bool sigma{}, local{}, emissive{}, pbr{}, foreground{}, wasActive{};
    nrd::Instance *instance{};
    std::vector<id<MTLTexture>> permanent, transient;
    struct Pipeline {
      id<MTLComputePipelineState> state = nil;
      Stage stage{};
      ArgumentSet resources, common;
    };
    std::vector<Pipeline> pipelines;
    id<MTLSamplerState> samplers[2] = {};
    // Per frame slot: every dispatch's constants and argument buffers.
    id<MTLBuffer> constants[gpuFrameCount] = {}, arguments[gpuFrameCount] = {};
    NSUInteger constantStride = 0, argumentStride = 0;
    const nrd::DispatchDesc *dispatches{};
    uint32_t dispatchCount{};
    ~NrdPass() {
      if (instance)
        nrd::DestroyInstance(*instance);
    }
  };

  struct Renderer::Gpu {
    id<MTLDevice> device = nil;
    uint32_t width = 0, height = 0;
    std::array<id<MTLTexture>, ImageCount> images{};
    id<MTLBuffer> uniforms[gpuFrameCount] = {};
    uint32_t frameIndex = 0;
    // denoise.comp, one variant per pass mode (scripts/shaders/metal.py):
    // a mode's variant reads the images it does not write as plain textures.
    static constexpr uint32_t modeCount = 6;
    id<MTLComputePipelineState> post[modeCount] = {};
    Stage postStage[modeCount] = {};
    ArgumentSet postSet[modeCount];
    Frame frame{};
    bool localFireflyFilter = false, initialized = false, submittedFrame = false;
    bool sigma{}, local{}, validation{}, localReconstruction{};
    remixapi_ShadowStatisticsEXT shadowStats{.structSize = sizeof(shadowStats)};
    uint32_t quietFrames = 0, warmupFrames = 32;
    float smoothedFrameMilliseconds = 0;
    LocalTuning localTuning;
    remixapi_DenoisingStatisticsEXT stats{.structSize = sizeof(stats)};
    std::unique_ptr<NrdPass> indirect, shadow, localDirect, pbr, foreground;

    id<MTLTexture> texture(MTLPixelFormat format, uint32_t w, uint32_t h) {
      // Render target usage only for the clear when history starts.
      auto t = texture2d(device, format, w, h,
                         MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget);
      stats.memoryBytes += t.allocatedSize;
      return t;
    }
    void initializeNrd(NrdPass &pass, bool sigma, bool local = false, bool pbr = false);
    void prepareNrd(NrdPass &pass, const scene::Matrix &projection, const scene::Matrix &previousProjection);
    void recordNrd(NrdPass &pass, id<MTLComputeCommandEncoder> encoder);
    void pass(id<MTLComputeCommandEncoder> encoder, uint32_t mode, uint32_t step = 0);
    std::vector<NrdPass *> nrdPasses() {
      return {indirect.get(), shadow.get(), localDirect.get(), pbr.get(), foreground.get()};
    }
  };

  Renderer::Renderer() = default;
  Renderer::~Renderer() = default;

  void Renderer::initialize(id<MTLDevice> device, uint32_t width, uint32_t height, bool reblur, bool sigma,
                            bool local, bool localDenoise, bool validation, bool localReconstruction,
                            bool foregroundDenoise, bool rayReconstruction) {
    m_gpu = std::make_unique<Gpu>();
    auto &g = *m_gpu;
    g.device = device;
    g.width = width;
    g.height = height;
    g.sigma = sigma;
    g.local = local;
    g.validation = validation;
    g.localReconstruction = localReconstruction;
    foregroundDenoise |= reblur;
    for (uint32_t i = 0; i < ImageCount; ++i) {
      if ((i >= Penumbra && i < SigmaImageCount && !sigma) || (i >= LocalInput && i < LocalImageCount && !local) ||
          (i >= EmissiveInput && i < EmissiveImageCount))
        continue;
      MTLPixelFormat pixel = MTLPixelFormatRGBA16Float;
      if (i == Normal || i == PreviousNormal || i == FrontNormal)
        pixel = MTLPixelFormatRGB10A2Unorm;
      if (i == Depth || i == PreviousDepth || i == Confidence || i == RrHitDistance || i == FrontDepth ||
          i == FrontConfidence || i == LocalConfidence)
        pixel = MTLPixelFormatR32Float;
      if (i == PsrMirror)
        pixel = MTLPixelFormatRGBA32Uint;
      if (i == Motion || i == FrontMotion)
        pixel = MTLPixelFormatRGBA32Float;
      if (i == Penumbra)
        pixel = MTLPixelFormatR16Float;
      if (i == ShadowInput || i == ShadowOutput)
        pixel = MTLPixelFormatRGBA8Unorm;
      uint32_t w = width, h = height;
      if ((i >= RrDiffuse && i < FrontNormal && !rayReconstruction) || (i >= FrontNormal && !foregroundDenoise) ||
          (i >= PbrSpecularSh1 && !reblur))
        w = h = 1;
      if (i == GradientA || i == GradientB || ((i == MovingA || i == MovingB) && reblur)) {
        w = (width + 4) / 5;
        h = (height + 4) / 5;
      } else if (i == MovingA || i == MovingB)
        w = h = 1;
      g.images[i] = g.texture(pixel, w, h);
    }
    for (auto &uniform : g.uniforms)
      uniform = buffer(device, nullptr, sizeof(Frame));
    const Stage first = local ? (sigma ? Stage::denoiseLocalSigmaMode0 : Stage::denoiseLocalMode0)
                              : (sigma ? Stage::denoiseSigmaMode0 : Stage::denoiseMode0);
    for (uint32_t mode = 0; mode < Gpu::modeCount; ++mode) {
      g.postStage[mode] = Stage(uint32_t(first) + mode);
      g.post[mode] = computePipeline(device, g.postStage[mode]);
      g.postSet[mode] = ArgumentSet(device, g.postStage[mode], 1);
    }
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
    }
    if (sigma) {
      g.shadow = std::make_unique<NrdPass>();
      g.initializeNrd(*g.shadow, true);
    }
    g.stats.peakMemoryBytes = g.stats.memoryBytes;
  }

  // denoise/nrd.cpp initializeNrd.
  void Renderer::Gpu::initializeNrd(NrdPass &pass, bool sigmaPass, bool localPass, bool pbrPass) {
    pass.sigma = sigmaPass;
    pass.local = localPass;
    pass.pbr = pbrPass;
    // Indirect light is denoised as SH and resolved per pixel (denoise.comp):
    // legacy diffuse, and both lobes of PBR surfaces. The translucent front
    // layer and the direct local-light pass stay plain radiance.
    const nrd::DenoiserDesc denoiser{
        0, sigmaPass ? nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY
           : pbrPass ? (pass.foreground ? nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR
                                        : nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR_SH)
           : localPass ? nrd::Denoiser::REBLUR_DIFFUSE
                       : nrd::Denoiser::REBLUR_DIFFUSE_SH};
    nrd::InstanceCreationDesc create{};
    create.denoisers = &denoiser;
    create.denoisersNum = 1;
    require(nrd::CreateInstance(create, pass.instance));
    const auto &desc = *nrd::GetInstanceDesc(*pass.instance);
    if (desc.resourcesSpaceIndex != 0 || desc.constantBufferAndSamplersSpaceIndex != 1)
      throw Error("Unexpected NRD register spaces");
    for (uint32_t i = 0; i < 2; ++i) {
      auto *sampler = [MTLSamplerDescriptor new];
      sampler.minFilter = sampler.magFilter = i ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
      sampler.mipFilter = MTLSamplerMipFilterNearest;
      sampler.sAddressMode = sampler.tAddressMode = sampler.rAddressMode = MTLSamplerAddressModeClampToEdge;
      sampler.supportArgumentBuffers = YES;
      pass.samplers[i] = [device newSamplerStateWithDescriptor:sampler];
      check(pass.samplers[i], "NRD sampler");
    }
    pass.pipelines.resize(desc.pipelinesNum);
    NSUInteger arguments = 0;
    for (uint32_t i = 0; i < desc.pipelinesNum; ++i) {
      const auto &source = desc.pipelines[i];
      if (!remix::denoise::pipelineRequired(source.shaderIdentifier, sigmaPass, localPass, pbrPass, validation,
                                            localReconstruction))
        continue;
      const shaders::NrdShader *shader = nullptr;
      for (const auto &s : shaders::nrdShaders)
        if (!std::strcmp(source.shaderIdentifier, s.identifier))
          shader = &s;
      if (!shader)
        continue;
      auto &p = pass.pipelines[i];
      p.stage = shader->stage;
      p.state = computePipeline(device, p.stage);
      p.resources = ArgumentSet(device, p.stage, 0);
      p.common = ArgumentSet(device, p.stage, 1);
      arguments = std::max(arguments, p.resources.encodedLength() + p.common.encodedLength());
    }
    const auto &pd = desc.descriptorPoolDesc;
    // Argument buffers and constant buffers: 256-byte aligned offsets.
    pass.argumentStride = (arguments + 511) & ~NSUInteger(255);
    pass.constantStride = (desc.constantBufferMaxDataSize + 255) & ~NSUInteger(255);
    for (unsigned i = 0; i < gpuFrameCount; ++i) {
      pass.constants[i] = buffer(device, nullptr, pass.constantStride * pd.setsMaxNum);
      pass.arguments[i] = buffer(device, nullptr, pass.argumentStride * pd.setsMaxNum);
    }
    auto allocate = [&](auto &pool, const nrd::TextureDesc *textures, uint32_t count) {
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t d = textures[i].downsampleFactor;
        pool.push_back(texture(format(textures[i].format), (width + d - 1) / d, (height + d - 1) / d));
      }
    };
    allocate(pass.permanent, desc.permanentPool, desc.permanentPoolSize);
    allocate(pass.transient, desc.transientPool, desc.transientPoolSize);
  }

  void Renderer::selectFrame(uint32_t slot) { m_gpu->frameIndex = slot % gpuFrameCount; }

  id<MTLTexture> Renderer::image(ImageSlot slot) const { return m_gpu->images.at(slot); }
  void Renderer::setLocalTuning(const LocalTuning &tuning) { m_gpu->localTuning = tuning; }

  // denoise/renderer.cpp begin's set 1: the base images, the final world and
  // sky, then the optional images at their slot + 4.
  void Renderer::bindInputs(ArgumentSet &set) const {
    const auto &g = *m_gpu;
    for (uint32_t i = 0; i < BaseImageCount; ++i)
      set.texture(i, g.images[i]);
    set.buffer(inputUniformBinding, g.uniforms[g.frameIndex]);
    for (uint32_t i = Penumbra; i < ImageCount; ++i)
      if (g.images[i])
        set.texture(i + 4, g.images[i]);
  }

  void Renderer::begin(id<MTLCommandBuffer> command, const Frame &frame, const scene::Matrix &projection,
                       const scene::Matrix &previousProjection, id<MTLTexture> world, id<MTLTexture> sky,
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
    // them with its prepass on (prepareNrd): shaders and settings agree.
    g.frame.translucency[3] =
        g.frame.flags[1] && g.frame.hitDistance[3] == 0 && !(localFireflyFilter && g.frame.local[1]);
    if (!g.initialized) {
      // History starts from zero, as vkCmdClearColorImage.
      std::vector<id<MTLTexture>> textures;
      for (auto t : g.images)
        if (t)
          textures.push_back(t);
      for (auto *pass : g.nrdPasses())
        if (pass) {
          textures.insert(textures.end(), pass->permanent.begin(), pass->permanent.end());
          textures.insert(textures.end(), pass->transient.begin(), pass->transient.end());
        }
      for (auto t : textures) {
        auto *clear = [MTLRenderPassDescriptor renderPassDescriptor];
        clear.colorAttachments[0].texture = t;
        clear.colorAttachments[0].loadAction = MTLLoadActionClear;
        clear.colorAttachments[0].storeAction = MTLStoreActionStore;
        clear.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        [[command renderCommandEncoderWithDescriptor:clear] endEncoding];
      }
      g.initialized = true;
    }
    std::memcpy(g.uniforms[g.frameIndex].contents, &g.frame, sizeof(Frame));
    for (auto &set : g.postSet) {
      set.begin(g.frameIndex);
      bindInputs(set);
      set.texture(BaseImageCount, world);
      set.texture(BaseImageCount + 1, sky);
      set.texture(BaseImageCount + 2, g.images[0]);
    }
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
      if (g.frame.shadow[3] && g.frame.local[0] && g.frame.local[1])
        g.prepareNrd(*g.localDirect, projection, previousProjection);
      else
        g.localDirect->wasActive = false;
    }
    if (g.foreground) {
      if (g.frame.translucency[0])
        g.prepareNrd(*g.foreground, projection, previousProjection);
      else
        g.foreground->wasActive = false;
    }
    if (g.sigma)
      g.prepareNrd(*g.shadow, projection, previousProjection);
  }

  // denoise/nrd.cpp prepareNrd.
  void Renderer::Gpu::prepareNrd(NrdPass &pass, const scene::Matrix &projection,
                                 const scene::Matrix &previousProjection) {
    nrd::CommonSettings common{};
    std::copy(projection.begin(), projection.end(), common.viewToClipMatrix);
    std::copy(previousProjection.begin(), previousProjection.end(), common.viewToClipMatrixPrev);
    // Our projections include Vulkan's framebuffer Y flip; NRD's UV Y is
    // opposite to clip Y (denoise/nrd.cpp).
    for (int column = 0; column < 4; ++column) {
      common.viewToClipMatrix[column * 4 + 1] *= -1;
      common.viewToClipMatrixPrev[column * 4 + 1] *= -1;
    }
    std::copy(frame.view.begin(), frame.view.end(), common.worldToViewMatrix);
    std::copy(frame.previousView.begin(), frame.previousView.end(), common.worldToViewMatrixPrev);
    for (int i = 0; i < 2; ++i) {
      common.cameraJitter[i] = frame.jitter[i];
      common.cameraJitterPrev[i] = frame.jitter[i + 2];
      const uint16_t size = uint16_t(i ? height : width);
      common.resourceSize[i] = common.resourceSizePrev[i] = common.rectSize[i] = common.rectSizePrev[i] = size;
      common.motionVectorScale[i] = 1.f / size;
    }
    common.motionVectorScale[2] = 1;
    common.timeDeltaBetweenFrames = frame.timing[0];
    common.frameIndex = uint32_t(stats.renderedFrames);
    common.accumulationMode =
        (frame.flags[0] || ((pass.sigma || pass.foreground) && frame.shadow[0]) || !pass.wasActive)
            ? nrd::AccumulationMode::CLEAR_AND_RESTART
            : nrd::AccumulationMode::CONTINUE;
    common.enableValidation = frame.timing[1] != 0;
    common.isHistoryConfidenceAvailable = !pass.sigma;
    common.enableValidation &= !pass.sigma && !pass.local && !pass.foreground;
    require(nrd::SetCommonSettings(*pass.instance, common));
    pass.wasActive = true;
    if (pass.sigma) {
      nrd::SigmaSettings settings{};
      std::copy_n(frame.sun, 3, settings.lightDirection);
      settings.maxStabilizedFrameNum =
          std::clamp(nrd::GetMaxAccumulatedFrameNum(nrd::SIGMA_DEFAULT_ACCUMULATION_TIME,
                                                    1000.f / std::max(smoothedFrameMilliseconds, 1.f)),
                     1u, nrd::SIGMA_MAX_HISTORY_FRAME_NUM);
      require(nrd::SetDenoiserSettings(*pass.instance, 0, &settings));
    } else {
      nrd::ReblurSettings settings{};
      // denoise/nrd.cpp: indirect light (legacy SH and PBR SH) follows
      // NRD-Sample's settings. Material IDs: PBR dielectric 0, metal 1,
      // legacy 2; specular keeps legacy surfaces out.
      const bool indirect = !pass.local && !pass.foreground;
      settings.minMaterialForDiffuse = 0;
      settings.minMaterialForSpecular = pass.foreground ? 0.f : 1.f;
      settings.fastHistoryClampingSigmaScale = 1.5f;
      if (indirect)
        settings.lobeAngleFraction *= 1.333f; // the SG resolve regains normal detail
      if (frame.local[1]) {
        settings.fireflySuppressorMinRelativeScale = 1.5f;
        settings.diffusePrepassBlurRadius = 0.f;
      }
      settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
      settings.hitDistanceParameters = {frame.hitDistance[0], frame.hitDistance[1], frame.hitDistance[2]};
      settings.maxAccumulatedFrameNum =
          std::clamp(nrd::GetMaxAccumulatedFrameNum(nrd::REBLUR_DEFAULT_ACCUMULATION_TIME,
                                                    1000.f / std::max(smoothedFrameMilliseconds, 1.f)),
                     1u, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
      if (pass.pbr) {
        settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
        settings.diffusePrepassBlurRadius = 4.f;
        settings.specularPrepassBlurRadius = 8.f;
        if (localFireflyFilter && frame.local[1]) {
          settings.diffusePrepassBlurRadius = 0.f;
          settings.usePrepassOnlyForSpecularMotionEstimation = true;
        }
        settings.maxBlurRadius = 16.f;
        if ((frame.local[0] || pass.foreground) && frame.local[2])
          settings.maxAccumulatedFrameNum = std::min(settings.maxAccumulatedFrameNum, 8u);
      }
      if (pass.foreground) {
        settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
        settings.diffusePrepassBlurRadius = 2.f;
        settings.specularPrepassBlurRadius = 4.f;
        settings.usePrepassOnlyForSpecularMotionEstimation = false;
        settings.fireflySuppressorMinRelativeScale = 3.f;
        settings.fastHistoryClampingSigmaScale = 3.f;
      }
      if (pass.local) {
        const bool responsive = frame.local[2] != 0;
        settings.diffusePrepassBlurRadius = 0.f;
        settings.minBlurRadius = .5f;
        settings.maxBlurRadius = 8.f;
        settings.historyFixBasePixelStride = 2;
        settings.historyFixFrameNum = 1;
        settings.maxAccumulatedFrameNum =
            std::min(settings.maxAccumulatedFrameNum, responsive ? 2u : localTuning.historyFrames);
        settings.antilagSettings.luminanceSigmaScale = localTuning.antilagSigma;
        settings.antilagSettings.luminanceSensitivity = localTuning.antilagSensitivity;
        settings.maxFastAccumulatedFrameNum = 2;
        settings.maxStabilizedFrameNum = responsive ? 1u : 8u;
      } else
        warmupFrames = std::max(32u, settings.maxAccumulatedFrameNum);
      if (indirect && localTuning.indirectHistoryFrames)
        settings.maxAccumulatedFrameNum = std::min(settings.maxAccumulatedFrameNum, localTuning.indirectHistoryFrames);
      if (indirect) {
        // A fast history five times shorter than the main one, stabilized
        // over the main history's length.
        settings.maxFastAccumulatedFrameNum = settings.maxAccumulatedFrameNum / 5;
        settings.maxStabilizedFrameNum = settings.maxAccumulatedFrameNum;
      }
      settings.maxFastAccumulatedFrameNum =
          std::min(settings.maxFastAccumulatedFrameNum, settings.maxAccumulatedFrameNum);
      require(nrd::SetDenoiserSettings(*pass.instance, 0, &settings));
    }
    const nrd::Identifier id = 0;
    require(nrd::GetComputeDispatches(*pass.instance, &id, 1, pass.dispatches, pass.dispatchCount));
  }

  // denoise/nrd.cpp recordNrd: one argument buffer pair and constant block per dispatch.
  void Renderer::Gpu::recordNrd(NrdPass &pass, id<MTLComputeCommandEncoder> encoder) {
    const auto &desc = *nrd::GetInstanceDesc(*pass.instance);
    const auto &offset = nrd::GetLibraryDesc()->spirvBindingOffsets;
    if (pass.dispatchCount > desc.descriptorPoolDesc.setsMaxNum)
      throw Error("NRD dispatch limit exceeded");
    auto resolve = [&](const nrd::ResourceDesc &r) -> id<MTLTexture> {
      using R = nrd::ResourceType;
      switch (r.type) {
      case R::PERMANENT_POOL:
        return pass.permanent.at(r.indexInPool);
      case R::TRANSIENT_POOL:
        return pass.transient.at(r.indexInPool);
      case R::IN_PENUMBRA:
        return images[Penumbra];
      case R::IN_TRANSLUCENCY:
        return images[ShadowInput];
      case R::OUT_SHADOW_TRANSLUCENCY:
        return images[ShadowOutput];
      case R::IN_MV:
        return images[pass.foreground ? FrontMotion : Motion];
      case R::IN_NORMAL_ROUGHNESS:
        return images[pass.foreground ? FrontNormal : Normal];
      case R::IN_VIEWZ:
        return images[pass.foreground ? FrontDepth : Depth];
      case R::IN_DIFF_RADIANCE_HITDIST:
        return images[pass.foreground ? FrontDiffuse : pass.pbr ? Sh0 : LocalInput];
      case R::OUT_DIFF_RADIANCE_HITDIST:
        return images[pass.foreground ? FrontDiffuseOutput : pass.pbr ? PbrDiffuseOutput : LocalOutput];
      case R::IN_SPEC_RADIANCE_HITDIST:
        return images[pass.foreground ? FrontSpecular : PbrSpecularInput];
      case R::OUT_SPEC_RADIANCE_HITDIST:
        return images[pass.foreground ? FrontSpecularOutput : PbrSpecularOutput];
      case R::IN_DIFF_SH0:
        return images[Sh0];
      case R::IN_DIFF_SH1:
        return images[Sh1];
      case R::IN_SPEC_SH0:
        return images[PbrSpecularInput];
      case R::IN_SPEC_SH1:
        return images[PbrSpecularSh1];
      case R::IN_DIFF_CONFIDENCE:
      case R::IN_SPEC_CONFIDENCE:
        return images[pass.foreground ? FrontConfidence : pass.local ? LocalConfidence : Confidence];
      case R::OUT_DIFF_SH0:
        return images[pass.pbr ? PbrDiffuseOutput : Out0];
      case R::OUT_DIFF_SH1:
        return images[pass.pbr ? PbrDiffuseOutputSh1 : Out1];
      case R::OUT_SPEC_SH0:
        return images[PbrSpecularOutput];
      case R::OUT_SPEC_SH1:
        return images[PbrSpecularOutputSh1];
      case R::OUT_VALIDATION:
        return images[Validation];
      default:
        throw Error("Unbound NRD resource");
      }
    };
    auto constants = pass.constants[frameIndex], arguments = pass.arguments[frameIndex];
    for (uint32_t i = 0; i < pass.dispatchCount; ++i) {
      const auto &d = pass.dispatches[i];
      auto &p = pass.pipelines.at(d.pipelineIndex);
      if (!p.state)
        throw Error("Uncompiled NRD shader requested");
      const NSUInteger base = pass.argumentStride * i;
      p.resources.begin(arguments, base);
      p.common.begin(arguments, base + ((p.resources.encodedLength() + 255) & ~NSUInteger(255)));
      uint32_t t = 0, u = 0;
      for (uint32_t r = 0; r < d.resourcesNum; ++r) {
        const auto &resource = d.resources[r];
        const bool storage = resource.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
        p.resources.texture(desc.resourcesBaseRegisterIndex +
                                (storage ? offset.storageTextureAndBufferOffset + u++ : offset.textureOffset + t++),
                            resolve(resource));
      }
      for (uint32_t s = 0; s < desc.samplersNum; ++s)
        p.common.sampler(offset.samplerOffset + desc.samplersBaseRegisterIndex + s, pass.samplers[s]);
      if (d.constantBufferDataSize > pass.constantStride)
        throw Error("NRD constant limit exceeded");
      const NSUInteger position = pass.constantStride * i;
      if (d.constantBufferDataSize)
        std::memcpy(static_cast<char *>(constants.contents) + position, d.constantBufferData,
                    d.constantBufferDataSize);
      p.common.buffer(offset.constantBufferOffset + desc.constantBufferRegisterIndex, constants, position);
      [encoder setComputePipelineState:p.state];
      [encoder setBuffer:arguments offset:p.resources.offset() atIndex:0];
      [encoder setBuffer:arguments offset:p.common.offset() atIndex:1];
      p.resources.use(encoder);
      p.common.use(encoder);
      const auto &threads = library(p.stage).threads;
      [encoder dispatchThreadgroups:MTLSizeMake(d.gridWidth, d.gridHeight, 1)
              threadsPerThreadgroup:MTLSizeMake(threads[0], threads[1], threads[2])];
    }
  }

  // The post-processing shader's passes (denoise/shaders/denoise.comp).
  void Renderer::Gpu::pass(id<MTLComputeCommandEncoder> encoder, uint32_t mode, uint32_t step) {
    [encoder setComputePipelineState:post[mode]];
    [encoder setBuffer:postSet[mode].current() offset:0 atIndex:1];
    postSet[mode].use(encoder);
    // Variants that do not read pass.step have no push constants.
    const uint32_t push[2] = {mode, step};
    if (const auto index = library(postStage[mode]).push; index != 0xff)
      [encoder setBytes:push length:sizeof(push) atIndex:index];
    const uint32_t w = mode == 0 ? uint32_t(images[GradientA].width) : width;
    const uint32_t h = mode == 0 ? uint32_t(images[GradientA].height) : height;
    [encoder dispatchThreadgroups:MTLSizeMake((w + 7) / 8, (h + 7) / 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  }

  // Serial compute encoders order the dispatches as the Vulkan barriers do.
  void Renderer::recordOpaque(id<MTLCommandBuffer> command) {
    auto &g = *m_gpu;
    id<MTLComputeCommandEncoder> encoder = computeEncoder(command, @"Denoise opaque lighting", g.width, g.height);
    if (g.frame.flags[1]) {
      for (uint32_t i = 1; i <= 5; ++i)
        g.pass(encoder, 0, i);
      g.pass(encoder, 1);
    }
    if (g.frame.flags[1] && g.frame.shadow[3])
      g.recordNrd(*g.indirect, encoder);
    if (g.frame.flags[1] && g.frame.shadow[2])
      g.recordNrd(*g.pbr, encoder);
    if (g.sigma)
      g.recordNrd(*g.shadow, encoder);
    if (g.localDirect && g.frame.shadow[3] && g.frame.local[0] && g.frame.local[1])
      g.recordNrd(*g.localDirect, encoder);
    if (g.frame.translucency[1])
      g.pass(encoder, 2, 0);
    [encoder endEncoding];
  }

  void Renderer::record(id<MTLCommandBuffer> command) {
    auto &g = *m_gpu;
    id<MTLComputeCommandEncoder> encoder = computeEncoder(command, @"Denoise, compose and TAA", g.width, g.height);
    if (g.foreground && g.frame.translucency[0])
      g.recordNrd(*g.foreground, encoder);
    // Denoised composition and TAA; with the MetalFX denoiser (Ray
    // Reconstruction's place) only the filtered foreground joins its input.
    if (g.frame.hitDistance[3] == 0) {
      g.pass(encoder, 2, 1);
      g.pass(encoder, 3);
    } else if (g.frame.translucency[0])
      g.pass(encoder, 5);
    g.pass(encoder, 4);
    [encoder endEncoding];
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
    return result;
  }
  bool Renderer::foregroundActive() const { return m_gpu->frame.translucency[0] != 0; }
  remixapi_DenoisingStatisticsEXT Renderer::statistics() const { return m_gpu->stats; }
} // namespace remix::metal::denoise
