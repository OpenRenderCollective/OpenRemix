// The Metal path tracer's frame: pathtrace/renderer.cpp, rendering.cpp,
// gbuffer.cpp and tlas.cpp with Metal acceleration structures and passes.
#include "metal/tracer_gpu.h"
#include "pathtrace/shaders/blue_noise_data.h"
#include "metal/diagnostics.h"
#include "pathtrace/sparse_policy.h"
#include "pathtrace/volume_policy.h"
#include "scene/display.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace remix::metal {
  using namespace scene;
  using namespace metal::trace;
  using shaders::Stage;

  namespace {
    // gbuffer.cpp formats: position, normal, albedo, previous, stable depth,
    // emission, PBR. Pass 0 writes all but the stable depth (output size).
    constexpr MTLPixelFormat primaryFormats[7] = {
        MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA8Unorm,
        MTLPixelFormatRGBA32Float, MTLPixelFormatR32Float,    MTLPixelFormatRGBA16Float,
        MTLPixelFormatRGBA16Float};
    constexpr unsigned primaryAttachments[6] = {0, 1, 2, 3, 5, 6};
    constexpr MTLTextureUsage storage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    // Tracing set bindings (pathtrace/renderer.cpp initialize): 0/1 TLASes,
    // 2 surface table, 3 frame, 4 world, 5 transmission, 6 textures, 7 sky,
    // 8-14 primary images, 15 the material guide.
    constexpr uint32_t bindingTable = 2, bindingFrame = 3, bindingWorld = 4, bindingTransmission = 5,
                       bindingTextures = 6, bindingSky = 7, bindingPrimary = 8, bindingGuide = 15;
    // The lighting kernels' specialization constants by constant_id
    // (pathtrace.comp; pathtrace/local_lights.cpp requestLightingVariant):
    // bools at their id (local lights 0, guiding 1, small set 2, resampling 3,
    // split lighting 4, ray profile 7, separate indirect 9, shadow early out
    // 12), and the sparse launch mode (13, an integer) from bit 16. The other
    // switches are the per-dispatch `features` push constant.
    constexpr uint32_t constantLocal = 1u << 0, constantGuided = 1u << 1, constantSmallSet = 1u << 2,
                       constantResampling = 1u << 3, constantProfile = 1u << 7, constantSeparateIndirect = 1u << 9,
                       constantEarlyOut = 1u << 12, constantSparseShift = 16;
    // pathtrace/gpu.h Feature.
    constexpr uint32_t featureDecals = 1, featureRoulette = 2, featureAngularEmitters = 8, featureFirstOpaqueHit = 16,
                       featureMultiSheetFallback = 32, defaultFeatures = featureDecals;
    // The push constant block holding `features` (offset 28).
    struct FeaturesPush {
      uint32_t padding[7] = {};
      uint32_t bits = defaultFeatures;
    };
  } // namespace

  PathTracer::PathTracer() = default;
  PathTracer::~PathTracer() = default;

  PathTracer::Configuration PathTracer::Configuration::of(void *devicePointer, const TraceOptions &options,
                                                         bool frameGeneration, bool local, bool perspective,
                                                         uint32_t displayWidth, uint32_t displayHeight,
                                                         uint32_t displayFormat) {
    // pathtrace/temporal.cpp beginFrame and configureFrame, and
    // pathtrace/sharc.cpp beginSharc.
    id<MTLDevice> device = (__bridge id<MTLDevice>)devicePointer;
    Configuration c;
    c.displayWidth = displayWidth;
    c.displayHeight = displayHeight;
    c.displayFormat = displayFormat;
    const bool temporal = !options.accumulate && !options.sharcDebug;
    // Reconstruction: MetalFX takes the upscaler's place; Ray Reconstruction's
    // setting selects its denoised scaler, which replaces NRD and TAA.
    using reconstruction::Backend;
    auto backend = Backend::Native;
    if (options.upscaler != upscale::Mode::Off && temporal) {
      std::string reason;
      if (!perspective)
        c.reconstructionUnavailable = "Reconstruction requires a finite perspective world camera";
      else if (options.dlssRayReconstruction && reconstruction::denoisingSupported(device, reason))
        backend = Backend::MetalFXDenoising;
      else if (reconstruction::upscalingSupported(device, c.reconstructionUnavailable))
        backend = Backend::MetalFXUpscaling;
      if (options.dlssRayReconstruction && backend != Backend::MetalFXDenoising && c.reconstructionUnavailable.empty())
        c.reconstructionUnavailable = reason;
    }
    c.reconstruction = uint32_t(backend);
    const bool denoised = backend == Backend::MetalFXDenoising;
    c.reblur = options.denoiser && temporal && !denoised;
    c.taa = options.taa && temporal && backend == Backend::Native;
    c.sigma = options.sigma && temporal && !denoised;
    c.local = local && (c.reblur || c.taa || c.sigma || backend != Backend::Native) && !denoised;
    c.localDenoise = c.local && options.localDenoiser && c.reblur;
    c.validation = options.validation;
    c.width = displayWidth;
    c.height = displayHeight;
    if (backend != Backend::Native)
      reconstruction::renderSize(device, backend,
                                 options.upscaler == upscale::Mode::Fsr31 ? options.fsrQuality : options.dlssQuality,
                                 displayWidth, displayHeight, c.width, c.height);
    c.frameGeneration = frameGeneration && c.temporal();
    c.earlyOut = options.shadowEarlyOut;
    // pathtrace/gpu.h indirectPlanned (mode 2, the default).
    const char *split = std::getenv("REMIX_SPLIT_INDIRECT");
    c.separateIndirect = !(split && !std::strcmp(split, "0")) && options.bounces != 0 && !options.sparseRendering;
#if REMIX_HAS_SHARC
    c.sharc = options.sharc && !options.accumulate && options.bounces;
    c.sharcCapacityLog2 = c.sharc ? options.sharcCapacityLog2 : 0;
#endif
    return c;
  }

  const PathTracer::Configuration &PathTracer::configuration() const { return m_gpu->configuration; }

  void PathTracer::initialize(void *device, const Configuration &configuration) {
    m_gpu = std::make_unique<Gpu>();
    auto &g = *m_gpu;
    g.device = (__bridge id<MTLDevice>)device;
    g.configuration = configuration;
    g.width = configuration.width;
    g.height = configuration.height;
    g.opacityCache = scene::OpacityCache(8, std::getenv("REMIX_OPACITY_PROFILE") != nullptr);
    g.createPipelines();
    g.createComposite(MTLPixelFormat(configuration.displayFormat));
    g.createTarget();
    g.post = std::make_unique<post::Renderer>();
    g.post->initialize(g.device, configuration.displayWidth, configuration.displayHeight, g.width, g.height);
    const auto &c = configuration;
    const auto backend = reconstruction::Backend(c.reconstruction);
    // pathtrace/temporal.cpp beginFrame: the denoiser owns the temporal guides
    // whenever any temporal pass, reconstruction included, consumes them.
    if (c.temporal()) {
      g.denoiser = std::make_unique<denoise::Renderer>();
      const bool denoised = backend == reconstruction::Backend::MetalFXDenoising;
      g.denoiser->initialize(g.device, c.width, c.height, c.reblur, c.sigma, c.local, c.localDenoise, c.validation,
                             false, c.reblur || denoised, denoised);
    }
    // Frame interpolation reuses the input preparation without MetalFX scaling.
    if (backend != reconstruction::Backend::Native || c.frameGeneration) {
      g.reconstruction = std::make_unique<reconstruction::Renderer>();
      g.reconstruction->initialize(g.device, backend, c.width, c.height, c.displayWidth, c.displayHeight,
                                   MTLPixelFormat(c.displayFormat));
    }
    g.beginSharc();
  }

  // pathtrace/renderer.cpp createRawPipelines, temporal.cpp beginFrame and
  // sharc.cpp beginSharc: this configuration's lighting passes.
  // pathtrace/sharc.cpp lightingStage: SHARC runs on Metal (through its
  // lock buffer), so the cache-querying stages are the only lighting family,
  // as on a SHARC-capable Vulkan device; the query is gated per frame.
  Stage PathTracer::Gpu::lightingStage(Stage uncached) {
#if REMIX_HAS_SHARC
    switch (uncached) {
    case Stage::trace: return Stage::sharcQuery;
    case Stage::temporal: return Stage::sharcTemporal;
    case Stage::transparency: return Stage::sharcTransparency;
    case Stage::temporalTransparency: return Stage::sharcTemporalTransparency;
    case Stage::singleTransparency: return Stage::sharcSingleTransparency;
    case Stage::singleTemporalTransparency: return Stage::sharcSingleTemporalTransparency;
    case Stage::confidence: return Stage::sharcConfidence;
    case Stage::sigmaTemporal: return Stage::sharcSigmaTemporal;
    case Stage::localTemporal: return Stage::sharcLocalTemporal;
    case Stage::localSigmaTemporal: return Stage::sharcLocalSigmaTemporal;
    case Stage::indirectTemporal: return Stage::sharcIndirectTemporal;
    case Stage::indirect: return Stage::sharcIndirect;
    case Stage::sheetLighting: return Stage::sharcSheetLighting;
    case Stage::sheetLightingTemporal: return Stage::sharcSheetLightingTemporal;
    default: return uncached;
    }
#else
    return uncached;
#endif
  }

  // pathtrace/renderer.cpp createRawPipelines and temporal.cpp beginFrame:
  // this configuration's passes, their base pipelines compiled concurrently
  // (Apple's compiler service takes 5-15 s per lighting kernel). The
  // multi-sheet pass and primary surface replacement compile in the
  // background; their frames compose without them meanwhile.
  void PathTracer::Gpu::createPipelines() {
    const auto &c = configuration;
    const bool temporal = c.temporal();
    struct Request {
      Lighting *pass;
      Stage stage;
      LightingFlags flags;
    };
    std::vector<Request> requests;
    const auto add = [&](Lighting &pass, Stage stage, LightingFlags flags) {
      requests.push_back({&pass, stage, flags});
    };
    if (temporal) {
      add(opaque, lightingStage(c.local && c.sigma ? Stage::localSigmaTemporal
                                : c.local          ? Stage::localTemporal
                                : c.sigma          ? Stage::sigmaTemporal
                                                   : Stage::temporal),
          {true, true, true, c.separateIndirect});
      add(transparency, lightingStage(Stage::temporalTransparency), {false, true, true, false, true});
      add(singleTransparency, lightingStage(Stage::singleTemporalTransparency), {false});
      add(sheet, lightingStage(Stage::sheetLightingTemporal), {false});
      add(frontGuides, Stage::frontGuides, {false, true, false});
      add(psr, Stage::psr, {false, true, false, false, true});
      if (c.reblur)
        add(confidence, lightingStage(Stage::confidence), {false});
      if (c.separateIndirect)
        add(indirect, lightingStage(Stage::indirectTemporal), {true});
    } else {
      add(opaque, lightingStage(Stage::trace), {true, true, true, c.separateIndirect});
      add(transparency, lightingStage(Stage::transparency), {false});
      add(singleTransparency, lightingStage(Stage::singleTransparency), {false});
      add(sheet, lightingStage(Stage::sheetLighting), {false});
      if (c.separateIndirect)
        add(indirect, lightingStage(Stage::indirect), {true});
    }
    if (c.sharc) {
      add(sharcUpdate, Stage::sharcUpdate, {false, false});
      add(sharcResolve, Stage::sharcResolve, {false, false, false});
    }
    // ReGIR's presampling and the volumetric update compile in the
    // background; their passes start once they are ready.
    add(regirPresample, Stage::regirPresample, {false, false, false, false, true});
    add(volumeUpdate, Stage::volumeUpdate, {false, false, false, false, true});
    // Sparse rendering's passes likewise; frames are lit densely meanwhile.
    if (c.reconstruction == uint32_t(reconstruction::Backend::MetalFXDenoising)) {
      add(sparseCompact, Stage::sparseCompact, {false, false, false, false, true});
      add(sparseArgs, Stage::sparseArgs, {false, false, false, false, true});
    }
    std::vector<std::thread> compiling;
    std::vector<std::string> failures(requests.size());
    for (size_t i = 0; i < requests.size(); ++i) {
      createLighting(*requests[i].pass, requests[i].stage, requests[i].flags);
      if (requests[i].flags.background)
        continue;
      compiling.emplace_back([&, i] {
        @autoreleasepool {
          try {
            computePipeline(device, requests[i].pass->stage, requests[i].pass->base);
          } catch (const std::exception &error) {
            failures[i] = error.what();
          }
        }
      });
    }
    for (auto &thread : compiling)
      thread.join();
    for (const auto &failure : failures)
      if (!failure.empty())
        throw Error(failure);
    primaryVertexSet = ArgumentSet(device, Stage::gbufferVert, 0);
    primaryFragmentSet = ArgumentSet(device, Stage::gbufferFrag, 0);
    compositeSet = ArgumentSet(device, Stage::compositeFrag, 0);
    postSet = ArgumentSet(device, Stage::compositeFrag, 1);
    NSError *error = nil;
    {
      auto *descriptor = [MTLRenderPipelineDescriptor new];
      descriptor.label = @"Primary G-buffer";
      descriptor.vertexFunction = function(device, Stage::gbufferVert);
      descriptor.fragmentFunction = function(device, Stage::gbufferFrag);
      for (unsigned i = 0; i < 6; ++i)
        descriptor.colorAttachments[i].pixelFormat = primaryFormats[primaryAttachments[i]];
      descriptor.depthAttachmentPixelFormat = Raster::Gpu::depthFormat;
      primaryPipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
      check(primaryPipeline, "primary pipeline", error);
      // Reverse depth: cleared to 0, nearer is greater.
      auto *depth = [MTLDepthStencilDescriptor new];
      depth.depthCompareFunction = MTLCompareFunctionGreaterEqual;
      depth.depthWriteEnabled = YES;
      primaryDepth = [device newDepthStencilStateWithDescriptor:depth];
      // The stable depth: reconstruction's composite depth, and the raster
      // primary surface's depth (pathtrace.comp primaryDepth).
      {
        auto *stable = [MTLRenderPipelineDescriptor new];
        stable.label = @"Stable depth";
        stable.vertexFunction = function(device, Stage::gbufferVert);
        stable.fragmentFunction = function(device, Stage::depthFrag);
        stable.colorAttachments[0].pixelFormat = primaryFormats[4];
        stable.depthAttachmentPixelFormat = Raster::Gpu::depthFormat;
        stableDepthPipeline = [device newRenderPipelineStateWithDescriptor:stable error:&error];
        check(stableDepthPipeline, "stable depth pipeline", error);
        stableVertexSet = ArgumentSet(device, Stage::gbufferVert, 0);
        stableFragmentSet = ArgumentSet(device, Stage::depthFrag, 0);
      }
    }
  }

  // pathtrace/sharc.cpp createLightingPipelines: the base pipeline takes
  // local lights without guidance, the shadow early-out setting and its
  // separate indirect role; the scene's other variants follow on first use.
  void PathTracer::Gpu::createLighting(Lighting &pass, Stage stage, LightingFlags flags) {
    pass.stage = stage;
    pass.supportsGuiding = flags.supportsGuiding;
    pass.supportsRoulette = flags.supportsRoulette;
    pass.variants = flags.variants;
    pass.baseSeparateIndirect = flags.separateIndirect;
    pass.base = constantLocal | (configuration.earlyOut ? constantEarlyOut : 0u) |
                (flags.separateIndirect ? constantSeparateIndirect : 0u);
    if (flags.background)
      computePipelineAsync(device, stage, pass.base);
    pass.set0 = ArgumentSet(device, stage, 0);
    if (library(stage).sets > 1)
      pass.set1 = ArgumentSet(device, stage, 1);
  }

  PathTracer::Gpu::Selected PathTracer::Gpu::select(const Lighting &pass, bool separateIndirect, uint32_t sparse) {
    const auto base = [&] { return computePipelineAsync(device, pass.stage, pass.base); };
    if (!pass.variants)
      return {base(), defaultFeatures};
    const auto &s = lightingState;
    const bool guiding = pass.supportsGuiding;
    // The small set's sampler costs a transparency pass 255 registers: only
    // the opaque (guiding-capable) passes take it.
    Selected out;
    out.features = (s.decals ? featureDecals : 0u) | (s.roulette && pass.supportsRoulette ? featureRoulette : 0u) |
                   (s.local && s.angular && guiding ? featureAngularEmitters : 0u) |
                   (s.firstHit && guiding ? featureFirstOpaqueHit : 0u);
    const uint32_t constants = (s.local ? constantLocal : 0u) | (s.guided && guiding ? constantGuided : 0u) |
                               (s.local && s.smallSet && guiding ? constantSmallSet : 0u) |
                               (s.resampling && guiding ? constantResampling : 0u) |
                               (s.profile ? constantProfile : 0u) |
                               (separateIndirect ? constantSeparateIndirect : 0u) |
                               (s.earlyOut ? constantEarlyOut : 0u) | sparse << constantSparseShift;
    if (constants == pass.base) {
      out.pipeline = base();
      return out;
    }
    out.pipeline = computePipelineAsync(device, pass.stage, constants);
    // A consumer base stands in for its own variants (plain sampling); a role
    // that depends on its producer, or a sparse launch, has no fallback.
    if (!out.pipeline && !sparse && !(separateIndirect && !pass.baseSeparateIndirect))
      out.pipeline = base();
    return out;
  }

  // The display composite (pathtrace/renderer.cpp createTarget): the first
  // draw multiplies by colored transmission, the second writes radiance and
  // the traced depth for later raster overlays.
  void PathTracer::Gpu::createComposite(MTLPixelFormat format) {
    displayFormat = format;
    for (unsigned i = 0; i < 2; ++i) {
      auto *descriptor = [MTLRenderPipelineDescriptor new];
      descriptor.label = i ? @"Composite radiance" : @"Composite transmission";
      descriptor.vertexFunction = function(device, Stage::compositeVert);
      descriptor.fragmentFunction = function(device, Stage::compositeFrag);
      auto *color = descriptor.colorAttachments[0];
      color.pixelFormat = format;
      color.blendingEnabled = YES;
      color.sourceRGBBlendFactor = i ? MTLBlendFactorOne : MTLBlendFactorZero;
      color.destinationRGBBlendFactor = i ? MTLBlendFactorZero : MTLBlendFactorSourceColor;
      color.sourceAlphaBlendFactor = MTLBlendFactorOne;
      color.destinationAlphaBlendFactor = MTLBlendFactorZero;
      descriptor.depthAttachmentPixelFormat = Raster::Gpu::depthFormat;
      NSError *error = nil;
      composite[i] = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
      check(composite[i], "composite pipeline", error);
      auto *depth = [MTLDepthStencilDescriptor new];
      depth.depthCompareFunction = MTLCompareFunctionAlways;
      depth.depthWriteEnabled = !i;
      compositeDepth[i] = [device newDepthStencilStateWithDescriptor:depth];
    }
  }

  void PathTracer::Gpu::createTarget() {
    for (auto image : {world, transmission, sky})
      retire(image);
    for (auto image : primary)
      retire(image);
    world = texture2d(device, MTLPixelFormatRGBA32Float, width, height, storage);
    transmission = texture2d(device, MTLPixelFormatRGBA32Uint, width, height, storage);
    sky = texture2d(device, MTLPixelFormatRGBA32Float, width, height, MTLTextureUsageRenderTarget | storage);
    // The stable depth (primary 4) is at output size: reconstruction's
    // composite depth for raster overlays (pathtrace/gbuffer.cpp).
    for (unsigned i = 0; i < 7; ++i)
      primary[i] = texture2d(device, primaryFormats[i], i == 4 ? configuration.displayWidth : width,
                             i == 4 ? configuration.displayHeight : height, MTLTextureUsageRenderTarget | storage);
    history.clear();
    stats.accumulationCount = 0;
  }

  void PathTracer::selectFrame(uint32_t slot) {
    auto &g = *m_gpu;
    g.frameIndex = slot % gpuFrameCount;
    auto &f = g.frame();
    f.retired.clear();
    f.retiredGeometry.clear();
    f.retiredBuffers.clear();
    f.retiredStates.clear();
    g.retiredPreviousVertices.clear();
    if (g.denoiser)
      g.denoiser->selectFrame(g.frameIndex);
    g.readSharc();
    g.readLightGuiding();
    g.readRayProfile();
    if (g.post)
      g.post->selectFrame(g.frameIndex);
  }

  // pathtrace/rendering.cpp PathTracer::prepareScene (without the opacity profile).
  scene::TraceFrame PathTracer::prepareScene(Raster &resources, const TraceOptions &options) const {
    auto &g = *m_gpu;
    if (!g.workers)
      g.workers = std::make_unique<WorkerPool>();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.epoch).count();
    const bool filterEmission = options.emissiveLights && options.localLights &&
                                (options.freezeEmissiveRadiance || options.stabilizeEmissiveLighting);
    g.statics.resetStatistics();
    if (options.hardwareFaceCulling != g.opacityCache.oneSidedOpaqueEnabled()) {
      g.opacityCache.setOneSidedOpaque(options.hardwareFaceCulling);
      g.statics.invalidate();
    }
    auto frame = scene::prepareSceneFrame(resources, &g.statics, g.opacityCache, g.workers.get(), false, seconds,
                                          filterEmission, g.surfaceEmissionCache);
    g.opacityCache.prune();
    auto &staged = frame.lighting;
    staged.combined = g.lightBvh;
    staged.history = g.emissionHistory;
    const auto mode = options.freezeEmissiveRadiance ? 2u : options.stabilizeEmissiveLighting ? 1u : 0u;
    staged.history.begin(resources.temporalEpoch().world, frame.seconds,
                         options.emissiveLights && options.localLights ? mode : 0u);
    auto lights = options.torchLights ? resources.sphereLights() : std::vector<scene::SphereLight>{};
    const auto sphereCount = lights.size();
    for (const auto &source : frame.flatEmitters()) {
      auto light = source.light;
      if (source.animated)
        staged.history.apply(light, source.layout);
      if (options.emissiveLights && light.radius > 1e-8f)
        lights.push_back(light);
    }
    staged.history.end();
    staged.hasEmissive = lights.size() > sphereCount;
    staged.combined.setClusterSize(options.emissiveClusterSize);
    staged.combined.update(std::move(lights));
    return frame;
  }

  void PathTracer::Gpu::buildTop(id<MTLCommandBuffer> command, const std::vector<remix::trace::InstanceInput> &staticInputs,
                                 const std::vector<remix::trace::InstanceInput> &dynamicInputs, bool force,
                                 bool faceCulling) {
    const std::vector<remix::trace::InstanceInput> *inputs[2] = {&staticInputs, &dynamicInputs};
    id<MTLAccelerationStructureCommandEncoder> encoder = nil;
    for (unsigned i = 0; i < 2; ++i) {
      auto &top = tlas[i];
      const auto reason = top.policy.reason(*inputs[i], i != 0, force && i != 0);
      const bool changed = reason || top.policy.changed(*inputs[i]) || (!top.handle && !inputs[i]->empty());
      if (i == 1)
        stats.dynamicRebuildReason = changed ? reason : 0;
      if (!changed)
        continue;
      retire(top.handle);
      retire(top.instances);
      top.handle = nil;
      top.instances = nil;
      top.blas.clear();
      if (!inputs[i]->empty()) {
        // Metal instances name their BLAS by index into the descriptor's list.
        std::unordered_map<uint64_t, uint32_t> indices;
        std::vector<MTLAccelerationStructureUserIDInstanceDescriptor> data(inputs[i]->size());
        for (size_t n = 0; n < inputs[i]->size(); ++n) {
          const auto &input = (*inputs[i])[n];
          auto [entry, inserted] = indices.try_emplace(input.geometry, uint32_t(top.blas.size()));
          if (inserted) {
            const auto accel = accels.at(input.geometry).lock();
            if (!accel || !accel->handle)
              throw Error("traced instance without a BLAS");
            top.blas.push_back(accel->handle);
          }
          auto &instance = data[n];
          for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 3; ++r)
              instance.transformationMatrix.columns[c].elements[r] = input.model[c * 4 + r];
          // Vulkan's instance flags are Metal's options: facing cull disable,
          // and flip facing (VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE)
          // as the counter-clockwise front face; both default to clockwise
          // fronts. pathtrace/shaders/gbuffer.frag's facing test, which matches
          // the Vulkan ray facing, agrees with these options.
          static_assert(remix::trace::instanceFacingCullDisable == MTLAccelerationStructureInstanceOptionDisableTriangleCulling &&
                        remix::trace::instanceFrontCounterClockwise ==
                            MTLAccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise);
          instance.options = MTLAccelerationStructureInstanceOptions(remix::trace::instanceFlags(input, faceCulling));
          // pathtrace/tlas.cpp: view models only under their own mask (camera
          // rays), reflection-only instances under theirs (bounce rays), blended
          // layers under theirs (secondary rays may skip them), decals under
          // theirs (shadow rays pass them).
          instance.mask = input.viewModel        ? remix::trace::viewModelInstanceMask
                          : input.reflectionOnly ? remix::trace::reflectionOnlyInstanceMask
                          : input.blended        ? remix::trace::blendedInstanceMask
                          : input.decal          ? remix::trace::decalInstanceMask
                                                 : remix::trace::allInstancesMask;
          instance.intersectionFunctionTableOffset = 0;
          instance.accelerationStructureIndex = entry->second;
          instance.userID = input.customIndex;
        }
        top.instances = metal::buffer(device, data.data(), data.size() * sizeof(data[0]));
        NSMutableArray<id<MTLAccelerationStructure>> *structures = [NSMutableArray arrayWithCapacity:top.blas.size()];
        for (auto structure : top.blas)
          [structures addObject:structure];
        auto *descriptor = [MTLInstanceAccelerationStructureDescriptor descriptor];
        descriptor.instancedAccelerationStructures = structures;
        descriptor.instanceCount = data.size();
        descriptor.instanceDescriptorBuffer = top.instances;
        descriptor.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeUserID;
        if (!encoder) {
          encoder = accelerationEncoder(command, @"TLAS builds");
        }
        top.handle = build(encoder, descriptor);
      }
      top.policy.commit(*inputs[i], reason);
      if (i == 0)
        ++stats.staticTlasBuilds;
      else
        ++stats.dynamicTlasBuilds;
    }
    if (encoder)
      [encoder endEncoding];
  }

  // pathtrace/rendering.cpp PathTracer::Gpu::prepare, raw tracing (no
  // denoisers, SHARC, light guiding, PBR reuse or a separate indirect pass).
  void PathTracer::Gpu::prepare(id<MTLCommandBuffer> frameCommand, Raster &resources,
                                const TraceOptions &requestedOptions, scene::TraceFrame &prepared) {
    // pathtrace/temporal.cpp effectiveOptions: the MetalFX denoiser (Ray
    // Reconstruction's place) replaces NRD, SIGMA, TAA and their inputs.
    auto options = requestedOptions;
    const bool denoised = reconstruction::Backend(configuration.reconstruction) ==
                          reconstruction::Backend::MetalFXDenoising;
    if (denoised) {
      options.denoiser = options.localDenoiser = options.sigma = options.taa = false;
      options.blueNoiseShadows = options.blueNoiseLocalLights = false;
      options.waterIndirectReuse = options.pbrLocalReuse = false;
    }
    // Acceleration structure builds and the tracing passes are command buffers
    // of their own, committed ahead of the frame's display commands on the
    // same queue, so their GPU times are measured (gpuBuild/TraceMilliseconds).
    id<MTLCommandBuffer> buildCommand = [frameCommand.commandQueue commandBuffer];
    id<MTLCommandBuffer> command = [frameCommand.commandQueue commandBuffer];
    buildCommand.label = @"Acceleration structure builds";
    command.label = @"Path tracing";
    struct Commit {
      id<MTLCommandBuffer> build, trace;
      std::shared_ptr<GpuTimes> times;
      ~Commit() {
        auto times = this->times;
        [build addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
          if (buffer.status == MTLCommandBufferStatusCompleted)
            times->build = float((buffer.GPUEndTime - buffer.GPUStartTime) * 1e3);
        }];
        [trace addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
          if (buffer.status == MTLCommandBufferStatusCompleted) {
            times->trace = float((buffer.GPUEndTime - buffer.GPUStartTime) * 1e3);
            times->available = true;
          }
        }];
        [build commit];
        [trace commit];
      }
    } commit{buildCommand, command, gpuTimes};
    auto &raster = resources.gpu();
    const bool faceCulling = opacityCache.oneSidedOpaqueEnabled();
    debugView = options.debugView;
    const bool falseColor = debugView != 0 && debugView != 6 && debugView != 7;
    // The false colour debug views are shown without the tone curve.
    falseColorView = falseColor;
    rendered = false;
    const auto &c = configuration;
    const bool rouletteActive = options.russianRoulette && options.rouletteMinBounces < options.bounces;
    const uint64_t integrator =
        options.samples | uint64_t(options.bounces) << 8 | uint64_t(options.accumulate) << 16 |
        uint64_t(bool(denoiser) && c.reblur) << 17 | uint64_t(bool(denoiser) && c.taa) << 18 |
        uint64_t(options.rasterPrimary) << 19 | uint64_t(bool(sharc.stats.active)) << 20 |
        uint64_t(options.sharcDebug) << 21 | uint64_t(options.russianRoulette) << 23 |
        uint64_t(options.rouletteMinBounces) << 24;
    pendingTime = std::chrono::steady_clock::now();
    history.begin(resources.temporalEpoch(), integrator, width, height);
    pendingVertices.clear();
    pendingVertices.reserve(previousVertices.size());
    remix::trace::SceneSignature signature;
    std::vector<uint8_t> *sceneBytes = &signature.head;
    auto append = [&](const void *data, size_t size) {
      const auto *bytes = static_cast<const uint8_t *>(data);
      sceneBytes->insert(sceneBytes->end(), bytes, bytes + size);
    };
    for (const auto &draw : resources.draws())
      if (draw.camera == REMIXAPI_CAMERA_TYPE_SKY) {
        append(draw.model.data(), sizeof(draw.model));
        const auto material = scene::TemporalHistory::materialSignature(draw);
        append(&material, sizeof(material));
        append(draw.surface->vertices.data(), draw.surface->vertices.size() * sizeof(scene::Vertex));
        append(draw.surface->indices.data(), draw.surface->indices.size() * sizeof(uint32_t));
        const auto &camera = resources.camera(REMIXAPI_CAMERA_TYPE_SKY);
        append(camera.data(), sizeof(camera));
      }
    sceneBytes = &signature.frame;
    if (!prepared.staticUnits && !resources.persistentStatic() && !statics.units().empty() &&
        statics.committedAbsent()) {
      tables.reset();
      statics.clear();
    }
    if (!workers)
      workers = std::make_unique<WorkerPool>();
    history.setFallback(&statics);
    TableViews views{this};
    const remix::trace::FrameTables<TableViews>::Inputs tableInputs{prepared,
                                                                    &statics,
                                                                    history,
                                                                    previousVertices,
                                                                    pendingVertices,
                                                                    *workers,
                                                                    ++prepareSerial,
                                                                    false,
                                                                    false,
                                                                    PathTracer::textureLimit,
                                                                    &materialTicks,
                                                                    &temporalTicks};
    tables.build(views, tableInputs);
    const auto flags = tables.flags();
    // Texture descriptors in first-use order (the static prefix's, then the rest).
    std::vector<id<MTLTexture>> textures;
    std::vector<id<MTLSamplerState>> samplers;
    // The slots of textures holding sRGB-encoded texels (frame.srgbTextures).
    std::vector<uint32_t> srgbSlots;
    {
      ScopedTicks time{materialTicks};
      auto sampled = [&](const remix::trace::DescriptorRequest &request) {
        const auto image = request.kind == remix::trace::DescriptorRequest::Albedo
                               ? raster.sampledMaterial(request.material)
                           : request.kind == remix::trace::DescriptorRequest::Map
                               ? raster.sampledTexture(request.material, *request.texture)
                               : raster.sampledTexture(request.material, request.material->emissionTexture);
        if (image.srgb)
          srgbSlots.push_back(uint32_t(textures.size()));
        textures.push_back(image.texture);
        samplers.push_back(image.sampler);
      };
      if (const auto *prefix = tables.prefix())
        for (const auto &request : prefix->requests)
          sampled(request);
      for (const auto &request : tables.tail().requests)
        sampled(request);
    }
    // Retire unused deformation views; rigid views live as long as their CPU mesh.
    auto retireView = [&](decltype(meshes)::iterator it) {
      frame().retiredGeometry.push_back(std::move(it->second.geometry));
      for (auto &vertices : it->second.vertices)
        frame().retiredBuffers.push_back(std::move(vertices));
      deformViews.erase(&it->first);
      return meshes.erase(it);
    };
    const uint64_t releases = scene::meshReleases.load(std::memory_order_acquire);
    if (releases != meshReleasesSeen) {
      meshReleasesSeen = releases;
      for (auto it = meshes.begin(); it != meshes.end();)
        it = it->second.owner.expired() || (std::get<1>(it->first) && it->second.lastUse != sequence)
                 ? retireView(it)
                 : std::next(it);
    } else if (!deformViews.empty()) {
      std::vector<const MeshViewKey *> unused;
      for (const auto *key : deformViews) {
        const auto found = meshes.find(*key);
        if (found != meshes.end() && (found->second.owner.expired() || found->second.lastUse != sequence))
          unused.push_back(key);
      }
      for (const auto *key : unused)
        retireView(meshes.find(*key));
    }
    for (auto it = updateViews.begin(); it != updateViews.end();) {
      auto view = meshes.find(it->second);
      it = view == meshes.end() || view->second.updateStream.get() != std::get<0>(it->first) ? updateViews.erase(it)
                                                                                            : std::next(it);
    }
    std::erase_if(accels, [](const auto &entry) { return entry.second.expired(); });
    buildBottom(buildCommand);
    buildTop(buildCommand, tables.staticInputs(), tables.dynamicInputs(), options.forceRebuild, faceCulling);
    // The surface table (an empty frame uploads one default surface).
    const TraceSurface emptySurface{};
    const size_t prefixSurfaceCount = tables.prefixSurfaces(), tailSurfaceCount = tables.tail().surfaces.size();
    const bool noSurfaces = !prefixSurfaceCount && !tailSurfaceCount;
    const size_t prefixBytes = prefixSurfaceCount * sizeof(TraceSurface);
    const size_t tableBytes = (noSurfaces ? 1 : prefixSurfaceCount + tailSurfaceCount) * sizeof(TraceSurface);
    tables.signature(signature);
    auto &f = frame();
    if (!f.table || f.table->size < tableBytes)
      f.table = buffer(nullptr, tableBytes);
    auto *table = static_cast<uint8_t *>(f.table->handle.contents);
    if (prefixBytes)
      std::memcpy(table, tables.prefix()->surfaces.data(), prefixBytes);
    std::memcpy(table + prefixBytes, noSurfaces ? &emptySurface : tables.tail().surfaces.data(),
                tableBytes - prefixBytes);
    // Lights.
    auto &lighting = prepared.lighting;
    emissionHistory = std::move(lighting.history);
    lightBvh = std::move(lighting.combined);
    if ((!lightNodes || lightBufferGeneration != lightBvh.generation) && !lightBvh.nodes().empty()) {
      lightBufferGeneration = lightBvh.generation;
      if (lightNodes)
        f.retiredBuffers.push_back(std::move(lightNodes));
      if (lightEmitters)
        f.retiredBuffers.push_back(std::move(lightEmitters));
      lightNodes = buffer(lightBvh.nodes().data(), lightBvh.nodes().size() * sizeof(scene::LightNode));
      lightEmitters = buffer(lightBvh.emitters().data(), lightBvh.emitters().size() * sizeof(scene::LightEmitter));
    }
    Frame frame = {};
    frame.localConfig[0] = options.localLights && options.localIntensity > 0 ? lightBvh.active : 0;
    frame.splitLocal[3] = debugView;
    frame.localConfig[1] = options.localSamples;
    frame.localConfig[2] = uint32_t(lightBvh.lightingGeneration);
    frame.localConfig[3] =
        uint32_t(lightBvh.lightingGeneration >> 32) | (options.adaptiveLocalSampling ? 0x80000000u : 0u) |
        (lighting.hasEmissive ? 0u : 0x40000000u); // sphere-only set: guide sphere lights too
    frame.localScale[0] = options.localIntensity;
    frame.localScale[1] = options.localRange;
    frame.localScale[2] = lightBvh.emitters().size() <= 4 ? 1.f
                          : lighting.hasEmissive        ? float(options.emissiveCandidates)
                                                        : float(options.sphereCandidates);
    frame.localScale[3] = options.localRange > 0 ? 1.f / (options.localRange * options.localRange) : 0;
    frame.camera = resources.camera();
    primaryReverseDepth = resources.reverseDepth();
    frame.inverseCamera = scene::inverse(frame.camera);
    const auto eye = scene::inverse(resources.view());
    std::copy_n(eye.data() + 12, 3, frame.eye);
    // Camera rays (pathtrace/rendering.cpp): the view and the projection
    // inverted separately, without the eye's translation, which single
    // precision loses far from the origin (near geometry would jitter).
    try {
      auto rotation = eye;
      rotation[12] = rotation[13] = rotation[14] = 0;
      frame.inverseCameraRelative = scene::multiply(rotation, scene::inverse(resources.projection()));
    } catch (...) {
      scene::Matrix toEye = scene::identity();
      for (int i = 0; i < 3; ++i)
        toEye[12 + i] = -frame.eye[i];
      frame.inverseCameraRelative = scene::multiply(toEye, frame.inverseCamera);
    }
    auto light = resources.light();
    if (options.overrideLight) {
      const float norm =
          std::hypot(options.lightDirection[0], options.lightDirection[1], options.lightDirection[2]);
      if (norm > 1e-6f)
        for (int i = 0; i < 3; ++i)
          light.direction[i] = options.lightDirection[i] / norm;
      for (int i = 0; i < 3; ++i)
        light.radiance[i] = options.lightColor[i] * options.lightIntensity;
    }
    const float diameter =
        options.sunAngularDiameter >= 0 ? options.sunAngularDiameter : light.angularDiameterDegrees;
    frame.sun[0] = std::tan(diameter * (3.14159265359f / 360.f));
    frame.sun[1] = denoiser && c.sigma;
    // pathtrace/blue_noise.cpp configureBlueNoise.
    {
      const bool sunNoise = options.blueNoiseShadows && frame.sun[1] && frame.sun[0] > 0;
      const bool localNoise = options.blueNoiseLocalLights && frame.localConfig[0] && denoiser && c.reblur &&
                              c.localDenoise && !options.accumulate;
      // First-surface bounce and sky directions: a repeating 32-frame pattern
      // is for the denoiser, never for a progressive reference.
      const bool skyNoise = options.blueNoiseSky && denoiser && c.reblur && !options.accumulate;
      if (sunNoise || localNoise || skyNoise) {
        if (!blueNoise)
          blueNoise = buffer(remix::trace::blueNoiseSamples, sizeof(remix::trace::blueNoiseSamples));
        frame.blueNoise = blueNoise->address;
        frame.jitter[3] = float(uint32_t(sunNoise) | uint32_t(localNoise) << 1 | uint32_t(skyNoise) << 2);
      }
    }
    frame.sun[2] = frame.sun[1] != 0 ? float(options.shadowDebug) : 0;
    std::copy_n(light.direction, 3, frame.direction);
    std::copy_n(light.radiance, 3, frame.radiance);
    frame.radiance[3] = light.volumetric; // the sun's scale in volumetric media (volume.glsl)
    if (options.sky)
      std::copy_n(resources.skyRadiance().data(), 3, frame.skyRadiance);
    if (options.sky && resources.dome().enabled) {
      const auto &dome = resources.dome();
      std::copy_n(dome.radiance.data(), 3, frame.skyRadiance);
      frame.skyRadiance[3] = -1;
      for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 3; ++col)
          frame.domeTransform[row][col] = dome.worldToLocal[col * 4 + row];
      if (dome.texture && textures.size() < PathTracer::textureLimit) {
        frame.skyRadiance[3] = float(textures.size() + 1);
        const auto image = raster.sampledTexture(resources.defaultMaterial(), dome.texture);
        if (image.srgb)
          srgbSlots.push_back(uint32_t(textures.size()));
        textures.push_back(image.texture);
        samplers.push_back(image.sampler);
        auto fingerprint = dome.texture->fingerprint();
        append(&fingerprint, sizeof(fingerprint));
      }
    }
    // Role CUT (remixapi_InstanceInfoMaskEXT): the coverage of the groups
    // whose instances the camera does not see, drawn by the raster renderer
    // ahead of this frame's passes. Its sources are in no table: their
    // digest stands for them in the scene signature.
    if (const auto &cut = raster.mask.cut; cut.count && textures.size() + cut.count <= PathTracer::textureLimit) {
      frame.cutConfig[0] = uint32_t(textures.size() + 1);
      frame.cutConfig[1] = cut.count;
      std::copy_n(cut.slots, 4, frame.cutSlots);
      for (uint32_t i = 0; i < cut.count; ++i) {
        textures.push_back(raster.mask.targets[cut.targets[i]].texture);
        samplers.push_back(raster.coverageSampler);
      }
      append(&cut.digest, sizeof(cut.digest));
    }
    frame.config[0] = options.samples;
    frame.config[1] = options.bounces;
    frame.target[0] = width;
    frame.target[1] = height;
    const bool staticInstances = !tables.staticInputs().empty(), dynamicInstances = !tables.dynamicInputs().empty();
    frame.target[2] = uint32_t(staticInstances) | uint32_t(dynamicInstances) << 1 | uint32_t(faceCulling) << 2;
    frame.target[3] = options.rasterPrimary;
    // Reserved word, in scene-change detection (pathtrace/rendering.cpp).
    frame.jitter[2] = options.waterIndirectReuse && options.indirectClamp == 0 && denoiser && c.taa &&
                      !options.accumulate && options.samples == 1 && options.bounces > 0;
    frame.firefly[0] = options.localDirectClamp;
    frame.firefly[1] = options.indirectClamp;
    frame.firefly[2] = 1; // the exposure meter's material guide (meterMaterial)
    const bool roulette = rouletteActive;
    frame.pathConfig[0] = roulette;
    frame.pathConfig[1] = options.rouletteMinBounces;
    // Sheet flags (1 traced indirect, 2 local lights) and the sky rays at a
    // path's first surface (bits 8-15) and a sheet's (bits 16-23).
    frame.pathConfig[2] = (options.translucentIndirect ? 1u : 0u) | (options.sheetLocalLights ? 2u : 0u) |
                          (options.layerReuse ? 4u : 0u) | (options.shortSheetPaths ? 8u : 0u) |
                          std::clamp(options.skySamples, 1u, 16u) << 8 |
                          std::clamp(std::min(options.sheetSkySamples, options.skySamples), 1u, 16u) << 16;
    // Bits 0-7: the instance mask of shadow, light-visibility and bounce rays
    // (view models never; blended layers unless skipBlendedSecondaryRays is
    // off). Bit 8: a traced view model is in the frame (camera rays look for
    // it first).
    frame.pathConfig[3] = (options.skipBlendedSecondaryRays ? 0xffu & ~uint32_t(remix::trace::blendedInstanceMask) : 0xffu) &
                          ~uint32_t(remix::trace::viewModelInstanceMask) &
                          ~uint32_t(remix::trace::reflectionOnlyInstanceMask);
    if (prepared.viewModel)
      frame.pathConfig[3] |= 0x100u;
    std::copy_n(resources.cameraMedium().data(), 4, frame.cameraMedium);
    frame.upAxis = float(std::min(options.volumetricUpAxis, 2u));
    // pathtrace/rendering.cpp: light through translucent surfaces with a normal
    // map draws caustics (visibility.glsl causticTint).
    if (options.caustics && options.causticsStrength > 0) {
      const uint64_t strength = uint64_t(std::lround(std::clamp(options.causticsStrength, 0.f, 1.f) * 255));
      const uint64_t contrast = uint64_t(std::lround(std::clamp(options.causticsContrast, 0.f, 7.96875f) * 32));
      const uint64_t depth = uint64_t(remix::trace::detail::halfBits(options.causticsDepth / options.sceneScale));
      frame.caustics = strength | contrast << 8 | depth << 16;
    }

    // Classify the empty space just above a shading point (2 cm); paint lies
    // at most 5 cm above its receiver; texture footprints are in output
    // pixels (render/output size with reconstruction).
    frame.visibilityProbe[0] = .02f / options.sceneScale;
    frame.visibilityProbe[1] = .05f / options.sceneScale;
    frame.visibilityProbe[2] = c.displayHeight && height < c.displayHeight ? float(height) / float(c.displayHeight) : 1.f;
    static_assert(PathTracer::textureLimit <= 32 * 128);
    for (const uint32_t slot : srgbSlots)
      frame.srgbTextures[slot >> 7][(slot >> 5) & 3] |= 1u << (slot & 31);
    append(&frame, sizeof(frame));
    {
      // Scrolling maps (water): wrapped well before float precision suffers.
      // Set after the scene signature: the clock is not a change of the scene
      // (pathtrace/rendering.cpp hashes it, which restarts accumulation and
      // the reuse of an unchanged world's output every frame).
      static const auto start = std::chrono::steady_clock::now();
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      frame.animationTime = float(std::fmod(seconds, 4096.0));
    }
    append(&lightBvh.generation, sizeof(lightBvh.generation));
    // Dynamic textures (remixapi_UpdateTextureEXT): what their images show is
    // part of the scene through this count.
    {
      const uint64_t dynamicTextures = resources.dynamicTextureGeneration();
      append(&dynamicTextures, sizeof(dynamicTextures));
    }
    append(&integrator, sizeof(integrator));
    const auto temporalEpoch = resources.temporalEpoch();
    append(&temporalEpoch, sizeof(temporalEpoch));
    configureSharc(frame, options, temporalEpoch, tables.staticBytes(), tables.staticGeneration(),
                   tables.dynamicBytes(),
                   flags.receivers || (options.sharcGlossySurfaces && flags.sharpReceivers) ||
                       (options.sharcDynamicSurfaces &&
                        (flags.dynamicReceivers || (options.sharcGlossySurfaces && flags.dynamicSharpReceivers))));
    const bool sceneChanged = !signature.equal(lastScene);
    lastScene = std::move(signature);
    if (sceneChanged) {
      ++stats.sceneGeneration;
      stats.accumulationCount = 0;
    }
    if (options.accumulate != bool(stats.accumulationEnabled))
      stats.accumulationCount = 0;
    if (!options.accumulate || stats.accumulationCount >= 1048576)
      stats.accumulationCount = 0;
    // pathtrace/rendering.cpp: local lighting response and the temporal frame.
    const bool localBlueNoise = (uint32_t(frame.jitter[3]) & 2u) != 0;
    const bool localSettingsChanged = frame.localScale[0] != previousLocalIntensity ||
                                      frame.localScale[1] != previousLocalRange ||
                                      localBlueNoise != previousLocalBlueNoise;
    if (lightBvh.lightingGeneration != previousLocalGeneration || localSettingsChanged ||
        frame.localConfig[0] != previousLocalActive || (frame.localConfig[3] & 0x80000000u) != previousLocalAdaptive)
      localResponsiveFrames = 8;
    else if (localResponsiveFrames)
      --localResponsiveFrames;
    previousLocalGeneration = lightBvh.lightingGeneration;
    previousLocalIntensity = frame.localScale[0];
    previousLocalRange = frame.localScale[1];
    previousLocalBlueNoise = localBlueNoise;
    previousLocalActive = frame.localConfig[0];
    previousLocalAdaptive = frame.localConfig[3] & 0x80000000u;
    temporalFrame = {};
    const bool staticUnits = prepared.staticUnits;
    temporalFrame.shadow[2] =
        (staticUnits && statics.livePbrSurfaces()) ||
        std::any_of(resources.draws().begin(), resources.draws().end(),
                    [](const auto &draw) { return draw.traced() && draw.material->pbr; });
    temporalFrame.shadow[3] =
        (staticUnits && statics.liveSurfaces() > statics.livePbrSurfaces()) ||
        std::any_of(resources.draws().begin(), resources.draws().end(),
                    [](const auto &draw) { return draw.traced() && !draw.material->pbr; });
    temporalFrame.translucency[0] = denoiser && (c.reblur || denoised) && options.translucencyDenoiser && flags.blended;
    temporalFrame.translucency[1] = denoiser && c.reblur && options.translucencyReuse && options.bounces > 0 &&
                                    options.indirectClamp == 0 && !debugView && flags.blended;
    temporalFrame.local[0] = denoiser && c.localDenoise;
    temporalFrame.local[1] = frame.localConfig[0];
    temporalFrame.local[2] = localResponsiveFrames;
    temporalFrame.families[0] = frame.localConfig[0];
    temporalFrame.families[2] = localResponsiveFrames;
    // NRD's hit-distance scale A is in meters (3 m); B and C are unitless.
    temporalFrame.hitDistance[0] = 3.f / options.sceneScale;
    // Primary surface replacement (pathtrace/rendering.cpp): mirrors seen
    // through the rasterized primary surface, once its pipeline is ready.
    temporalFrame.translucency[2] = denoiser && psr && options.rasterPrimary && options.primarySurfaceReplacement &&
                                    computePipelineAsync(device, psr.stage, psr.base);
    // Surfaces that move on their own (pathtrace/rendering.cpp).
    temporalFrame.motion[0] = options.movingHistoryStart;
    temporalFrame.motion[1] = options.movingHistoryEnd;
    temporalFrame.motion[2] = options.movingIndirectHistory;
    temporalFrame.motion[3] = options.movingLocalHistory;
    temporalFrame.motionGradient[0] = options.movingNeighborHistory;
    temporalFrame.motionGradient[1] = options.movingGradients;
    temporalFrame.motionGradient[2] = options.movingLightTolerance;
    temporalFrame.motionGradient[3] = options.sceneScale / options.movingLightScale;
    temporalFrame.motionNoise[0] = options.movingGradientNoise;
    temporalFrame.motionNoise[1] = options.taaMotionRate;
    temporalFrame.view = resources.view();
    temporalFrame.inverseCamera = frame.inverseCamera;
    pendingCamera = frame.camera;
    pendingProjection = resources.projection();
    temporalFrame.previousView = history.reset() ? resources.view() : previousView;
    temporalFrame.previousCamera = history.reset() ? frame.camera : previousCamera;
    temporalFrame.previousInverseCamera = scene::inverse(temporalFrame.previousCamera);
    auto halton = [](uint32_t n, uint32_t base) {
      float value = 0, scale = 1.f / base;
      while (n) {
        value += float(n % base) * scale;
        n /= base;
        scale /= base;
      }
      return value - .5f;
    };
    // Camera jitter needs a resolve: accumulation or TAA.
    if (options.accumulate || (denoiser && c.taa)) {
      temporalFrame.jitter[0] = halton(uint32_t(sequence % 16) + 1, 2);
      temporalFrame.jitter[1] = halton(uint32_t(sequence % 16) + 1, 3);
    }
    if (c.reconstruction)
      reconstruction::jitter(temporalFrame.jitter, sequence, width, c.displayWidth);
    // Ray Reconstruction guides and composed radiance (pathtrace.comp).
    temporalFrame.hitDistance[3] = denoised ? 1.f : 0.f;
    std::copy_n(temporalFrame.jitter, 2, jitter);
    std::copy_n(temporalFrame.jitter, 2, frame.jitter);
    temporalFrame.jitter[2] = history.reset() ? temporalFrame.jitter[0] : previousJitter[0];
    temporalFrame.jitter[3] = history.reset() ? temporalFrame.jitter[1] : previousJitter[1];
    temporalFrame.flags[0] = history.reset() || frame.sun[2] != previousSun[7];
    temporalFrame.flags[1] = c.reblur;
    temporalFrame.flags[2] = c.taa;
    temporalFrame.flags[3] = denoiser ? uint32_t(denoiser->statistics().renderedFrames % 2) : 0;
    temporalFrame.target[0] = width;
    temporalFrame.target[1] = height;
    temporalFrame.target[2] = previousSeed;
    temporalFrame.target[3] = options.samples;
    temporalFrame.timing[0] =
        previousTime.time_since_epoch().count()
            ? std::clamp(float(std::chrono::duration<double, std::milli>(pendingTime - previousTime).count()), 1.f,
                         1000.f)
            : 16.6667f;
    temporalFrame.timing[1] = options.validation;
    temporalFrame.timing[2] = sceneChanged;
    temporalFrame.timing[3] = float(debugView);
    for (int i = 0; i < 3; ++i) {
      temporalFrame.sun[i] = -frame.direction[i];
      pendingSun[i] = frame.direction[i];
      pendingSun[i + 3] = frame.radiance[i];
    }
    pendingSun[6] = diameter;
    pendingSun[7] = frame.sun[2];
    pendingSun[8] = (uint32_t(frame.jitter[3]) & 1u) != 0;
    temporalFrame.sun[3] = frame.sun[1];
    // Abrupt sun changes invalidate only SIGMA.
    float directionDelta = 0, radianceDelta = 0, previousRadiance = 0;
    for (int i = 0; i < 3; ++i) {
      directionDelta += (pendingSun[i] - previousSun[i]) * (pendingSun[i] - previousSun[i]);
      radianceDelta += std::abs(pendingSun[i + 3] - previousSun[i + 3]);
      previousRadiance += previousSun[i + 3];
    }
    temporalFrame.shadow[0] = directionDelta > .01f || radianceDelta > std::max(.1f, previousRadiance * .5f) ||
                              diameter != previousSun[6] || pendingSun[8] != previousSun[8];
    temporalFrame.shadow[1] = uint32_t(frame.sun[2]);
    frame.lightNodes = lightNodes ? lightNodes->address : 0;
    frame.lightEmitters = lightEmitters ? lightEmitters->address : 0;
    frame.config[2] = uint32_t(sequence++);
    if (const auto *seed = std::getenv("REMIX_PATH_SAMPLE_SEED"))
      frame.config[2] += uint32_t(std::strtoul(seed, nullptr, 10));
    frame.config[3] = uint32_t(stats.accumulationCount);
    configureIndirect(frame, options);
    {
      // The sheet lighting pass's transport.
      const size_t bytes = size_t(width) * height * 16;
      if (!sheetResults || sheetResults->size < bytes) {
        if (sheetResults)
          f.retiredBuffers.push_back(std::move(sheetResults));
        sheetResults = buffer(nullptr, bytes);
      }
      frame.sheetResults = sheetResults->address;
    }
    configureVolume(command, frame, options);
    configureSparse(command, frame, options);
    configureRegir(frame, options, frame.localConfig[0] != 0);
    configureLightGuiding(frame, options, frame.localScale[2] > 1);
    configureSkyGuiding(frame, options);
    // Lights that changed since the previous frame (pathtrace/rendering.cpp):
    // the local denoiser drops history where they changed the light.
    {
      static const std::vector<scene::MovedLight> none;
      const auto &moved = options.localHistoryConfidence ? lightBvh.moved() : none;
      f.movedLights = moved.empty() ? nullptr : buffer(moved.data(), moved.size() * sizeof(moved[0]));
      frame.movedLights = f.movedLights ? f.movedLights->address : 0;
      frame.visibilityConfig[1] = uint32_t(moved.size());
    }
    if (!f.uniform)
      f.uniform = buffer(nullptr, sizeof(frame));
    if (rayProfile.path) {
      if (!rayProfile.counters)
        rayProfile.counters = buffer(nullptr, 768);
      frame.reuseConfig[2] = uint32_t(rayProfile.counters->address);
      frame.reuseConfig[3] = uint32_t(rayProfile.counters->address >> 32);
    }
    std::memcpy(f.uniform->handle.contents, &frame, sizeof(frame));
    stats.samplesPerPixel = options.samples;
    stats.indirectBounces = options.bounces;
    stats.accumulationEnabled = options.accumulate;
    ++stats.accumulationCount;
    stats.sampleSequence = sequence;
    stats.redrawRequested = options.accumulate || debugView;
    if (textures.empty()) {
      const auto image = raster.sampledMaterial(resources.defaultMaterial());
      textures.push_back(image.texture);
      samplers.push_back(image.sampler);
    }
    textures.resize(PathTracer::textureLimit, textures.front());
    samplers.resize(PathTracer::textureLimit, samplers.front());
    collectResident();
    for (auto *pass : lightingPasses)
      if (*pass)
        bindTracing(*pass, textures, samplers);
    for (auto *set : {&primaryVertexSet, &primaryFragmentSet, &stableVertexSet, &stableFragmentSet}) {
      if (!*set)
        continue;
      set->begin(frameIndex);
      set->buffer(bindingTable, f.table->handle);
      set->buffer(bindingFrame, f.uniform->handle);
      set->textures(bindingTextures, textures, samplers);
    }
    // Sky raster: the clear color, then sky draws jittered like primary rays.
    {
      auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = sky;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(.025, .035, .055, 1);
      // The sky's depth is pass-local, at the render size.
      pass.depthAttachment.texture = texture2d(device, Raster::Gpu::depthFormat, width, height,
                                               MTLTextureUsageRenderTarget, 1, MTLStorageModeMemoryless);
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = MTLStoreActionDontCare;
      pass.depthAttachment.clearDepth = 1;
      id<MTLRenderCommandEncoder> encoder = renderEncoder(command, pass, @"Sky raster", width, height);
      raster.record(encoder, Target::Sky, false, jitter, width, height);
      [encoder endEncoding];
    }
    if (options.rasterPrimary)
      recordPrimaryPass(command, false);
    // pathtrace/gbuffer.cpp recordPrimary: pass 1 with raster primary
    // visibility (the lighting passes read it) or reconstruction, whose
    // composite writes it for the raster draws tested against the scene.
    // Without such draws this frame the composite's depth goes unread, and
    // the scene is not rasterized a second time at output size.
    if (options.rasterPrimary || (configuration.reconstruction && raster.testsSceneDepth()))
      recordPrimaryPass(command, true);
    // pathtrace/local_lights.cpp selectLightingPipeline's inputs.
    {
      auto &s = lightingState;
      s.local = frame.localConfig[0] != 0;
      s.guided = s.local && lightGuiding.active;
      s.smallSet = lightBvh.nodes().size() >= 3 && lightBvh.nodes().size() <= 7;
      s.resampling = s.local && frame.localScale[2] > 1;
      s.angular = lighting.hasEmissive && options.emissiveSolidAngleSampling;
      s.firstHit = s.local && !flags.blended;
      s.roulette = rouletteActive;
      s.decals = flags.decal;
      s.earlyOut = options.shadowEarlyOut;
      s.profile = rayProfile.path != nullptr;
    }
    const bool querySharc = sharc.stats.active && (sharc.hasReceivers || options.sharcDebug);
    beginRayProfile(command, frame);
    recordRegir(command);
    recordVolume(command);
    recordSparse(command);
    beginLightGuiding(command);
    beginSkyGuiding(command);
    if (sharcUpdate)
      recordSharc(command);
    if (denoiser) {
      denoiser->setLocalTuning({options.localAntilagSigma, options.localAntilagSensitivity, options.localHistoryFrames,
                                options.indirectHistoryFrames});
      denoiser->begin(command, temporalFrame, pendingProjection,
                      history.reset() ? pendingProjection : previousProjection, world, sky,
                      options.localFireflyFilter && (c.localDenoise || options.bounces > 0));
    }
    // pathtrace/rendering.cpp: the opaque pass consumes the separate indirect
    // pass's transport while both have compiled; otherwise it traces the
    // bounces inline (its consumer base alone reads cleared, black indirect).
    Selected opaqueSelected;
    if (indirectPass.active) {
      opaqueSelected = select(opaque, true);
      if (!opaqueSelected.pipeline)
        indirectPass.active = false;
    }
    if (!opaqueSelected.pipeline)
      opaqueSelected = select(opaque);
    if (!indirectPass.active && indirectPass.results && opaque.baseSeparateIndirect &&
        opaqueSelected.pipeline == computePipelineAsync(device, opaque.stage, opaque.base)) {
      id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Separate indirect clear");
      [blit fillBuffer:indirectPass.results->handle range:NSMakeRange(0, indirectPass.results->size) value:0];
      [blit endEncoding];
    }
    // Sparse rendering launches the opaque pass twice (the selected pixels,
    // the guides of the others); until both variants compile the frame is lit
    // densely. A changed rate restarts the denoiser's history.
    Selected sparseLit, sparseGuides;
    if (sparse.active && !indirectPass.active) {
      sparseLit = select(opaque, false, 1);
      sparseGuides = select(opaque, false, 2);
    }
    sparse.launched = sparseLit.pipeline && sparseGuides.pipeline;
    if (const float rate = sparse.launched ? sparse.rate : 1; rate != sparse.launchedRate) {
      sparse.launchedRate = rate;
      temporalFrame.flags[0] = 1;
    }
    remix::trace::g_sparseState = {options.sparseRendering, sparse.launched, sparse.rate,
                                   sparse.launched         ? ""
                                   : sparse.inactiveReason ? sparse.inactiveReason
                                                           : "its lighting variants are compiling"};
    if (denoiser && temporalFrame.translucency[2])
      dispatch(command, psr, select(psr), "Primary surface replacement", width, height);
    if (denoiser && temporalFrame.translucency[0])
      dispatch(command, frontGuides, select(frontGuides), "Translucency front guides", width, height);
    if (indirectPass.active)
      dispatch(command, indirect, select(indirect),
               querySharc ? "Trace indirect (separate pass, SHARC query)" : "Trace indirect (separate pass)", width,
               height);
    // One dispatch: surface shading, sun and local-light shadow rays,
    // emissive sampling, water and (unless separate) the indirect path.
    if (sparse.launched) {
      // The two launches write disjoint pixels.
      dispatch(command, opaque, sparseGuides, "Sparse rendering: guides of unlit pixels", width, height);
      dispatch(command, opaque, sparseLit,
               querySharc ? "Trace opaque, selected pixels: direct, sun, local, emissive, indirect+SHARC"
                          : "Trace opaque, selected pixels: direct, sun, local, emissive, indirect",
               width, height, sparse.count->handle, 16);
    } else
      dispatch(command, opaque, opaqueSelected,
               indirectPass.active ? "Trace opaque: direct, sun, local, emissive"
               : querySharc        ? "Trace opaque: direct, sun, local, emissive, indirect+SHARC"
                                   : "Trace opaque: direct, sun, local, emissive, indirect",
               width, height);
    if (denoiser && c.reblur)
      dispatch(command, confidence, select(confidence), "Trace indirect confidence (1/5 res)", (width + 4) / 5,
               (height + 4) / 5);
    if (denoiser)
      denoiser->recordOpaque(command);
    // The front sheet's light (direct, paths, reused GI) for the composition
    // passes; this frame's opaque radiance and denoised outputs are available
    // to its reuse.
    dispatch(command, sheet, select(sheet), "Sheet lighting (front sheet)", width, height);
    // The multi-sheet pass compiles in the background; until it is ready the
    // single-sheet pass composes layered pixels from the front sheet's light.
    const auto multiSheet = select(transparency);
    auto single = select(singleTransparency);
    if (!multiSheet.pipeline)
      single.features |= featureMultiSheetFallback;
    dispatch(command, singleTransparency, single, "Trace transparency (single sheet)", width, height);
    dispatch(command, transparency, multiSheet, "Trace transparency (multi-sheet)", width, height);
    finishRayProfile(command);
    if (denoiser)
      denoiser->record(command);
    finishLightGuiding(command);
    // pathtrace/rendering.cpp: the upscaler takes the composed radiance with
    // the denoiser's guides (and the RR guides for the MetalFX denoiser).
    if (reconstruction && denoiser) {
      reconstruction::Guides guides;
      if (denoised)
        guides = {denoiser->image(denoise::RrDiffuse), denoiser->image(denoise::RrSpecular),
                  denoiser->image(denoise::RrNormal), denoiser->image(denoise::RrHitDistance)};
      reconstruction->record(command, denoiser->image(denoise::Composed), denoiser->image(denoise::Depth),
                             denoiser->image(denoise::Motion), denoiser->image(denoise::Transmission),
                             pendingProjection, resources.view(), temporalFrame.jitter, temporalFrame.flags[0], guides);
    }
    if (sharcUpdate)
      finishSharc(command);
    // pathtrace/rendering.cpp PathTracer::prepare: exposure and bloom from the
    // reconstructed output, or the traced image; debug views bypass them.
    {
      const bool reconstructed = reconstruction && configuration.reconstruction && !debugView;
      post->record(command, reconstructed ? reconstruction->output() : world, options, resources.temporalEpoch(),
                   true, options.debugView || options.shadowDebug || options.sharcDebug || options.validation,
                   stats.sceneGeneration);
    }
    compositeSet.begin(frameIndex);
    compositeSet.texture(bindingWorld, world);
    compositeSet.texture(bindingTransmission, transmission);
    postSet.begin(frameIndex);
    postSet.buffer(0, post->exposure());
    postSet.texture(1, post->bloom(), metal::sampler(device, 1, 3, 3));
    postSet.buffer(2, post->settings());
    stats.peakAccelerationBytes = std::max(stats.accelerationBytes, stats.peakAccelerationBytes);
    rendered = true;
  }

  // pathtrace/gbuffer.cpp recordPrimary: pass 0 (render size, jittered, the
  // G-buffer) and pass 1 (the stable depth: output size, opaque draws).
  // View models (PrimaryDraw::viewModel) follow everything else on a cleared
  // depth buffer (a second render pass here): in front of the scene, ordered
  // by their own depth (gbuffer.vert, stableDepth bit 1).
  void PathTracer::Gpu::recordPrimaryPass(id<MTLCommandBuffer> command, bool stable) {
    const auto &c = configuration;
    const uint32_t w = stable ? c.displayWidth : width, h = stable ? c.displayHeight : height;
    bool viewModels = false;
    for (size_t i = 0; i < tables.primaryCount() && !viewModels; ++i)
      viewModels = tables.primary(i).viewModel;
    id<MTLTexture> depth = texture2d(device, Raster::Gpu::depthFormat, w, h, MTLTextureUsageRenderTarget, 1,
                                     MTLStorageModeMemoryless);
    for (int part = 0; part < (viewModels ? 2 : 1); ++part) {
      auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
      if (stable) {
        pass.colorAttachments[0].texture = primary[4];
        pass.colorAttachments[0].loadAction = part ? MTLLoadActionLoad : MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 0);
      } else
        for (unsigned i = 0; i < 6; ++i) {
          pass.colorAttachments[i].texture = primary[primaryAttachments[i]];
          pass.colorAttachments[i].loadAction = part ? MTLLoadActionLoad : MTLLoadActionClear;
          pass.colorAttachments[i].storeAction = MTLStoreActionStore;
          pass.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
        }
      // Reverse depth: cleared to 0, nearer is greater.
      pass.depthAttachment.texture = depth;
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = MTLStoreActionDontCare;
      pass.depthAttachment.clearDepth = 0;
      id<MTLRenderCommandEncoder> encoder = renderEncoder(
          command, pass,
          stable ? (part ? @"Stable depth (view models)" : @"Stable depth (raster, output res)")
                 : (part ? @"Primary G-buffer (view models)" : @"Primary G-buffer (raster)"),
          w, h);
      [encoder setViewport:{0, 0, double(w), double(h), 0, 1}];
      [encoder setRenderPipelineState:stable ? stableDepthPipeline : primaryPipeline];
      [encoder setDepthStencilState:primaryDepth];
      [encoder setCullMode:MTLCullModeNone];
      auto &vertexSet = stable ? stableVertexSet : primaryVertexSet;
      auto &fragmentSet = stable ? stableFragmentSet : primaryFragmentSet;
      [encoder setVertexBuffer:vertexSet.current() offset:0 atIndex:0];
      [encoder setFragmentBuffer:fragmentSet.current() offset:0 atIndex:0];
      vertexSet.use(encoder, MTLRenderStageVertex);
      fragmentSet.use(encoder, MTLRenderStageFragment);
      if (!resident.empty())
        [encoder useResources:resident.data()
                        count:resident.size()
                        usage:MTLResourceUsageRead
                       stages:MTLRenderStageVertex | MTLRenderStageFragment];
      const auto vertexPush = library(Stage::gbufferVert).push;
      const auto fragmentPush = library(stable ? Stage::depthFrag : Stage::gbufferFrag).push;
      for (size_t i = 0; i < tables.primaryCount(); ++i) {
        const auto &draw = tables.primary(i);
        if ((stable && draw.blended) || draw.viewModel != (part == 1) || draw.reflectionOnly)
          continue;
        PrimaryPush push{draw.model,
                         {stable ? 0.f : jitter[0], stable ? 0.f : jitter[1]},
                         draw.surfaceIndex,
                         uint32_t(stable) | uint32_t(draw.viewModel) << 1,
                         primaryReverseDepth};
        [encoder setVertexBytes:&push length:sizeof(push) atIndex:vertexPush];
        [encoder setFragmentBytes:&push length:sizeof(push) atIndex:fragmentPush];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:draw.count];
      }
      [encoder endEncoding];
    }
  }

  void PathTracer::prepare(void *command, Raster &resources, const TraceOptions &options, scene::TraceFrame &scene) {
    auto &g = *m_gpu;
    const auto start = std::chrono::steady_clock::now();
    g.prepare((__bridge id<MTLCommandBuffer>)command, resources, options, scene);
    g.performance.geometryMilliseconds = tickMilliseconds(g.geometryTicks);
    g.geometryTicks = 0;
    g.performance.recordMilliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    g.submittedFrame();
  }

  // pathtrace/temporal.cpp PathTracer::submitted: this prepared frame is the
  // committed history (Metal command buffers commit in order).
  void PathTracer::Gpu::submittedFrame() {
    if (!rendered)
      return;
    rendered = false;
    history.commit();
    if (lightGuiding.active) {
      lightGuiding.writeIndex ^= 1;
      lightGuiding.valid = true;
      ++lightGuiding.stats.renderedFrames;
    }
    if (skyGuiding.active) {
      skyGuiding.writeIndex ^= 1;
      skyGuiding.valid = true;
    }
    statics.committed(prepareSerial, frame().retiredStates);
    if (sharc.stats.active && sharc.hasReceivers) {
      ++sharc.stats.renderedFrames;
      ++sharc.warmFrames;
    }
    retiredPreviousVertices = std::move(previousVertices);
    previousVertices = std::move(pendingVertices);
    // The submitted transform exactly (inverting a rounded inverse adds motion).
    previousCamera = pendingCamera;
    previousView = temporalFrame.view;
    previousProjection = pendingProjection;
    std::copy_n(temporalFrame.jitter, 2, previousJitter);
    previousSeed = uint32_t(sequence - 1);
    previousTime = pendingTime;
    std::copy_n(pendingSun, 9, previousSun);
    if (denoiser)
      denoiser->submitted();
  }

  // The tracing descriptor set (pathtrace/rendering.cpp's writes) and, for
  // temporal stages, the denoiser's inputs with the final world and sky.
  void PathTracer::Gpu::bindTracing(Lighting &pass, const std::vector<id<MTLTexture>> &textures,
                                    const std::vector<id<MTLSamplerState>> &samplers) {
    auto &set = pass.set0;
    auto &f = frame();
    set.begin(frameIndex);
    set.acceleration(0, tlas[0].handle);
    set.acceleration(1, tlas[1].handle);
    set.buffer(bindingTable, f.table->handle);
    set.buffer(bindingFrame, f.uniform->handle);
    set.texture(bindingWorld, world);
    set.texture(bindingTransmission, transmission);
    set.textures(bindingTextures, textures, samplers);
    set.texture(bindingSky, sky);
    for (uint32_t i = 0; i < 7; ++i)
      set.texture(bindingPrimary + i, primary[i]);
    set.texture(bindingGuide, post->guide());
    if (pass.set1 && denoiser) {
      pass.set1.begin(frameIndex);
      denoiser->bindInputs(pass.set1);
      pass.set1.texture(denoise::BaseImageCount, world);
      pass.set1.texture(denoise::BaseImageCount + 1, sky);
      pass.set1.texture(denoise::BaseImageCount + 2, denoiser->image(denoise::Sh0));
    }
  }

  // One compute encoder per pass: Metal orders and synchronizes them like the
  // Vulkan barriers between the dispatches.
  void PathTracer::Gpu::dispatch(id<MTLCommandBuffer> command, const Lighting &pass, const Selected &selected,
                                 const char *label, uint32_t w, uint32_t h, id<MTLBuffer> indirectBuffer,
                                 NSUInteger offset) {
    if (!selected.pipeline)
      return;
    id<MTLComputeCommandEncoder> encoder = computeEncoder(command, @(label), w, h);
    id<MTLComputePipelineState> pipeline = selected.pipeline;
    [encoder setComputePipelineState:pipeline];
    // pathtrace/local_lights.cpp bindTrace: the per-dispatch switches.
    if (const auto index = library(pass.stage).push; index != 0xff) {
      const FeaturesPush push{.bits = selected.features};
      [encoder setBytes:&push length:sizeof(push) atIndex:index];
    }
    [encoder setBuffer:pass.set0.current() offset:0 atIndex:0];
    pass.set0.use(encoder);
    if (pass.set1) {
      [encoder setBuffer:pass.set1.current() offset:0 atIndex:1];
      pass.set1.use(encoder);
    }
    if (!resident.empty())
      [encoder useResources:resident.data() count:resident.size() usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    for (const auto &top : tlas)
      if (!top.blas.empty())
        [encoder useResources:top.blas.data() count:top.blas.size() usage:MTLResourceUsageRead];
    const auto &threads = library(pass.stage).threads;
    if (indirectBuffer)
      [encoder dispatchThreadgroupsWithIndirectBuffer:indirectBuffer
                                 indirectBufferOffset:offset
                                threadsPerThreadgroup:MTLSizeMake(threads[0], threads[1], threads[2])];
    else
      [encoder dispatchThreadgroups:MTLSizeMake((w + threads[0] - 1) / threads[0], (h + threads[1] - 1) / threads[1], 1)
              threadsPerThreadgroup:MTLSizeMake(threads[0], threads[1], threads[2])];
    [encoder endEncoding];
  }

  // pathtrace/sharc.cpp beginSharc: the cache's buffers. Metal has no 64-bit
  // atomic compare-exchange; the update pass serializes insertion with a
  // 32-bit lock per entry after the hash entries (pathtrace/shaders/sharc.glsl).
  void PathTracer::Gpu::beginSharc() {
    auto &s = sharc.stats;
    s.supported = REMIX_HAS_SHARC;
    if (!configuration.sharc)
      return;
    const uint32_t capacity = 1u << configuration.sharcCapacityLog2;
    sharc.hash = buffer(nullptr, uint64_t(capacity) * 12);
    sharc.accumulation = buffer(nullptr, uint64_t(capacity) * REMIX_SHARC_ACCUMULATION_STRIDE);
    sharc.resolved = buffer(nullptr, uint64_t(capacity) * REMIX_SHARC_RESOLVED_STRIDE);
    sharc.counters = buffer(nullptr, 16);
    for (auto &readback : sharc.readback)
      readback = metal::buffer(device, nullptr, 16);
    s.capacity = capacity;
    s.memoryBytes = sharc.hash->size + sharc.accumulation->size + sharc.resolved->size + 16 * (gpuFrameCount + 1);
    s.active = 1;
  }

  // The counters this slot's previous frame copied (its frame has completed).
  void PathTracer::Gpu::readSharc() {
    if (!sharc.pending[frameIndex])
      return;
    uint32_t counters[4];
    std::memcpy(counters, sharc.readback[frameIndex].contents, sizeof(counters));
    auto &s = sharc.stats;
    s.updatePaths = counters[0];
    s.queryAttempts = counters[1];
    s.queryHits = counters[2];
    s.occupiedEntries = counters[3];
    sharc.pending[frameIndex] = false;
  }

  // pathtrace/sharc.cpp configureSharc.
  void PathTracer::Gpu::configureSharc(Frame &frame, const TraceOptions &options, scene::TemporalEpoch currentEpoch,
                                       const std::vector<uint8_t> &geometry, uint64_t geometryGeneration,
                                       const std::vector<uint8_t> &dynamicGeometry, bool hasReceivers) {
    sharc.stats.requested = options.sharc;
    sharc.stats.debugView = options.sharcDebug;
    if (!sharc.stats.active)
      return;
    sharc.clear |= !sharc.hasReceivers || !hasReceivers;
    sharc.hasReceivers = hasReceivers;
    if (!hasReceivers) {
      auto &s = sharc.stats;
      s.updatePaths = s.queryAttempts = s.queryHits = s.occupiedEntries = 0;
    }
    const bool sameGeometry = geometryGeneration && geometryGeneration == sharc.geometryGeneration;
    const bool geometryChanged = !sameGeometry && sharc.geometry != geometry;
    // A rebased world changes the epoch, and the keys with it.
    sharc.clear |= sharc.epoch != currentEpoch || sharc.reset != options.sharcReset ||
                   sharc.scale != options.sharcSceneScale || sharc.bounces != options.bounces;
    sharc.epoch = currentEpoch;
    sharc.reset = options.sharcReset;
    sharc.scale = options.sharcSceneScale;
    sharc.bounces = options.bounces;
    if (geometryChanged)
      sharc.geometry = geometry;
    sharc.geometryGeneration = geometryGeneration;
    float light[9];
    std::copy_n(frame.direction, 3, light);
    std::copy_n(frame.radiance, 3, light + 3);
    std::copy_n(frame.skyRadiance, 3, light + 6);
    float oldEnergy = 0, newEnergy = 0, difference = 0, angle = 0;
    for (int i = 0; i < 3; ++i)
      angle += light[i] * sharc.light[i];
    for (int i = 3; i < 9; ++i) {
      oldEnergy += sharc.light[i];
      newEnergy += light[i];
      difference += std::abs(light[i] - sharc.light[i]);
    }
    sharc.clear |= difference > .25f * std::max(oldEnergy, newEnergy) ||
                   (std::max(oldEnergy, newEnergy) > 0 && angle < .999f);
    const bool localChanged =
        !std::equal(std::begin(frame.localConfig), std::end(frame.localConfig), sharc.localConfig) ||
        frame.localScale[0] != sharc.localIntensity || frame.localScale[1] != sharc.localRange;
    if (localChanged) {
      sharc.warmFrames = 0;
      sharc.clear |= sharc.localConfig[0] != 0 && frame.localConfig[0] == 0;
      sharc.clear |= frame.localScale[1] != sharc.localRange;
      std::copy_n(frame.localConfig, 4, sharc.localConfig);
      sharc.localIntensity = frame.localScale[0];
      sharc.localRange = frame.localScale[1];
    }
    const bool dynamicChanged = sharc.dynamicGeometry != dynamicGeometry;
    if (localChanged || difference > 1e-7f || angle < .9999999f || geometryChanged)
      sharc.responsiveFrames = 32;
    else if (sharc.responsiveFrames)
      --sharc.responsiveFrames;
    // Something moves in most frames of a game: its shadows in the cache get
    // a window of their own, not the few frames of a lighting change.
    if (dynamicChanged)
      sharc.motionFrames = 32;
    else if (sharc.motionFrames)
      --sharc.motionFrames;
    if (geometryChanged)
      sharc.warmFrames = 0;
    if (dynamicChanged)
      sharc.dynamicGeometry = dynamicGeometry;
    std::copy_n(light, 9, sharc.light);
    if (sharc.clear)
      sharc.warmFrames = 0;
    frame.sharcHash = sharc.hash->address;
    frame.sharcAccumulation = sharc.accumulation->address;
    frame.sharcResolved = sharc.resolved->address;
    frame.sharcCounters = sharc.counters->address;
    // The update grid: one path a cell of sharcDownscale pixels a side, and
    // of more above sharcMaxUpdatePaths, so the cost of the world-space cache
    // does not grow with the resolution.
    sharc.downscale = std::max(options.sharcDownscale, 1u);
    if (options.sharcMaxUpdatePaths)
      while (sharc.downscale < 16 && uint64_t((width + sharc.downscale - 1) / sharc.downscale) *
                                             ((height + sharc.downscale - 1) / sharc.downscale) >
                                         options.sharcMaxUpdatePaths)
        ++sharc.downscale;
    frame.sharcConfig[0] = uint32_t(hasReceivers) | uint32_t(options.sharcDynamicSurfaces) << 1 |
                           uint32_t(options.sharcGlossySurfaces) << 2 |
                           uint32_t(!options.sharcFootprintTest) << 3;
    frame.sharcConfig[1] = sharc.stats.capacity;
    frame.sharcConfig[2] = sharc.downscale;
    frame.sharcConfig[3] = options.sharcDebug;
    frame.sharcGrid[0] = float(options.sharcSceneScale);
    // While the lighting changes a cell's mean says nothing of what it should
    // hold next: the limit would only hold new light back.
    frame.sharcGrid[1] = sharc.responsiveFrames ? 0 : options.sharcFireflyClamp;
    frame.sharcGrid[2] = float(sharc.responsiveFrames ? std::min(options.sharcAccumulationFrames, 4u)
                               : sharc.motionFrames
                                   ? std::min(options.sharcAccumulationFrames, options.sharcMotionFrames)
                                   : options.sharcAccumulationFrames);
    frame.sharcGrid[3] = float(options.sharcStaleFrames);
    std::copy_n(sharc.clear ? frame.eye : sharc.previousEye, 3, frame.sharcPreviousCamera);
    std::copy_n(frame.eye, 3, sharc.previousEye);
  }

  // pathtrace/sharc.cpp recordSharc: clear on a reset, then the update
  // (sparse path tracing into the cache) and the per-entry resolve.
  void PathTracer::Gpu::recordSharc(id<MTLCommandBuffer> command) {
    if (!sharc.stats.active || !sharc.hasReceivers)
      return;
    id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"SHARC clear");
    if (sharc.clear) {
      for (const auto *buffer : {sharc.hash.get(), sharc.accumulation.get(), sharc.resolved.get()})
        [blit fillBuffer:buffer->handle range:NSMakeRange(0, buffer->size) value:0];
      sharc.clear = false;
      ++sharc.stats.resets;
    }
    [blit fillBuffer:sharc.counters->handle range:NSMakeRange(0, 16) value:0];
    [blit endEncoding];
    const uint32_t scale = sharc.downscale;
    dispatch(command, sharcUpdate, select(sharcUpdate), "SHARC update (trace)", (width + scale - 1) / scale,
             (height + scale - 1) / scale);
    dispatch(command, sharcResolve, select(sharcResolve), "SHARC resolve (entries)", sharc.stats.capacity, 1);
  }

  void PathTracer::Gpu::finishSharc(id<MTLCommandBuffer> command) {
    if (!sharc.stats.active || !sharc.hasReceivers)
      return;
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromBuffer:sharc.counters->handle sourceOffset:0 toBuffer:sharc.readback[frameIndex]
       destinationOffset:0 size:16];
    [blit endEncoding];
    sharc.pending[frameIndex] = true;
  }

  // pathtrace/rendering.cpp PathTracer::record: the composite draws.
  void PathTracer::record(void *encoderPointer) {
    auto &g = *m_gpu;
    id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)encoderPointer;
    // upscale/renderer.cpp composite: the reconstructed output at display size.
    const auto backend = reconstruction::Backend(g.configuration.reconstruction);
    if (g.reconstruction && backend != reconstruction::Backend::Native && !g.debugView) {
      g.reconstruction->composite(encoder, g.primary[4], g.post->exposure(), g.post->bloom(), metal::sampler(g.device, 1, 3, 3), g.post->settings(),
                                  g.frameIndex);
      return;
    }
    const auto &composite = library(Stage::compositeFrag);
    [encoder setViewport:{0, 0, double(g.configuration.displayWidth), double(g.configuration.displayHeight), 0, 1}];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setFragmentBuffer:g.compositeSet.current() offset:0 atIndex:0];
    [encoder setFragmentBuffer:g.postSet.current() offset:0 atIndex:1];
    g.compositeSet.use(encoder, MTLRenderStageFragment);
    g.postSet.use(encoder, MTLRenderStageFragment);
    for (uint32_t i = 0; i < 2; ++i) {
      // pathtrace.frag's constants: SDR.
      const scene::DisplayOutput display;
      struct {
        uint32_t additive, falseColor, width, height, display;
        float peakNits, paperWhiteNits;
      } push{i,
             uint32_t(g.falseColorView),
             g.configuration.displayWidth,
             g.configuration.displayHeight,
             uint32_t(display.hdr10),
             display.peakNits,
             display.paperWhiteNits};
      [encoder setRenderPipelineState:g.composite[i]];
      [encoder setDepthStencilState:g.compositeDepth[i]];
      [encoder setFragmentBytes:&push length:sizeof(push) atIndex:composite.push];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
  }

  remixapi_TracingStatisticsEXT PathTracer::statistics() const {
    auto result = m_gpu->stats;
    const auto &times = *m_gpu->gpuTimes;
    result.gpuBuildMilliseconds = times.build;
    result.gpuTraceMilliseconds = times.trace;
    result.gpuTimingsAvailable = times.available;
    return result;
  }
  remixapi_FramePerformanceEXT PathTracer::performance() const { return m_gpu->performance; }
  remixapi_DenoisingStatisticsEXT PathTracer::denoisingStatistics() const {
    if (m_gpu->denoiser)
      return m_gpu->denoiser->statistics();
    return {.structSize = sizeof(remixapi_DenoisingStatisticsEXT)};
  }
  remixapi_ShadowStatisticsEXT PathTracer::shadowStatistics() const {
    if (m_gpu->denoiser)
      return m_gpu->denoiser->shadowStatistics();
    return {.structSize = sizeof(remixapi_ShadowStatisticsEXT)};
  }
  remixapi_SharcStatisticsEXT PathTracer::sharcStatistics() const { return m_gpu->sharc.stats; }
  // pathtrace/temporal.cpp configureIndirect (mode 2): the bounces trace in
  // their own pass whenever the configuration built it and the frame has
  // bounces; the transport exists while a consumer base may run.
  void PathTracer::Gpu::configureIndirect(Frame &constants, const TraceOptions &options) {
    indirectPass.active = false;
    if (!indirect || !indirectPass.mode || options.bounces == 0 || options.sparseRendering)
      return;
    const size_t bytes = size_t(width) * height * 16;
    if (!indirectPass.results || indirectPass.results->size < bytes) {
      if (indirectPass.results)
        frame().retiredBuffers.push_back(std::move(indirectPass.results));
      indirectPass.results = buffer(nullptr, bytes);
    }
    constants.indirectResults = indirectPass.results->address;
    indirectPass.active = true;
  }

  // pathtrace/sparse.cpp configureSparse: a blue-noise share of the pixels is
  // lit each frame and the MetalFX denoised scaler (Ray Reconstruction's
  // place) rebuilds the rest.
  void PathTracer::Gpu::configureSparse(id<MTLCommandBuffer> command, Frame &constants, const TraceOptions &options) {
    auto &s = sparse;
    constants.sparseList = constants.sparseCount = 0;
    std::fill_n(constants.sparseConfig, 4, 0u);
    s.active = s.launched = false;
    s.rate = 1;
    const float rate = remix::trace::sparseSamplingRate(options, uint64_t(width) * height);
    const bool ready = sparseCompact && sparseArgs &&
                       computePipelineAsync(device, sparseCompact.stage, sparseCompact.base) &&
                       computePipelineAsync(device, sparseArgs.stage, sparseArgs.base);
    s.inactiveReason =
        !options.sparseRendering ? "not requested"
        : configuration.reconstruction != uint32_t(reconstruction::Backend::MetalFXDenoising)
            ? "needs the MetalFX denoiser (Ray Reconstruction)"
        : indirectPass.active ? "separate indirect pass is active"
        : options.accumulate  ? "accumulation is on"
        : constants.splitLocal[3] || constants.sun[3] != 0 || options.sharcDebug || options.debugView
            ? "a debug view is on"
        : rate > options.sparseMaxRate ? "the rate at this render resolution exceeds sparseMaxRate"
        : !ready                       ? "its passes are compiling"
                                       : nullptr;
    if (s.inactiveReason)
      return;
    const size_t bytes = size_t(width) * height * 4;
    if (!s.list || s.list->size < bytes) {
      if (s.list)
        frame().retiredBuffers.push_back(std::move(s.list));
      s.list = buffer(nullptr, bytes);
    }
    if (!s.count) {
      // The accumulator starts at zero; the arguments pass resets it after.
      s.count = buffer(nullptr, 32);
      std::memset(s.count->handle.contents, 0, 32);
    }
    const uint32_t levels = remix::trace::sparseLevels(rate);
    s.rate = float(levels) / 256;
    s.active = true;
    constants.sparseList = s.list->address;
    constants.sparseCount = s.count->address;
    constants.sparseConfig[0] = 1;
    constants.sparseConfig[1] = levels;
  }

  // pathtrace/sparse.cpp recordSparse: before the opaque pass, list the
  // selected pixels (one 16x16 group per tile) and publish their count as the
  // lighting launch's threadgroups.
  void PathTracer::Gpu::recordSparse(id<MTLCommandBuffer> command) {
    if (!sparse.active)
      return;
    dispatch(command, sparseCompact, select(sparseCompact), "Sparse rendering: select and list pixels", width, height);
    dispatch(command, sparseArgs, select(sparseArgs), "Sparse rendering: launch arguments", 1, 1);
  }

  // pathtrace/volume.cpp configureVolume: the medium (the game's fog or the
  // override), its grids' size and this frame's update constants.
  void PathTracer::Gpu::configureVolume(id<MTLCommandBuffer> command, Frame &constants, const TraceOptions &options) {
    auto &v = volume;
    constants.volumeGrid = 0;
    std::fill_n(constants.volumeConfig, 4, 0u);
    std::fill_n(constants.volumeFrame, 4, 0u);
    v.active = false;
    const auto medium = remix::trace::volumeMedium(options, remix::trace::gameFog());
    const bool ready = computePipelineAsync(device, volumeUpdate.stage, volumeUpdate.base) != nil;
    v.inactiveReason = !options.volumetrics ? "not requested"
                       : !medium.active     ? "the game sets no fog and the override is off"
                       : options.accumulate ? "accumulation is on"
                       : constants.splitLocal[3] || constants.sun[3] != 0 || options.sharcDebug || options.debugView
                           ? "a debug view is on"
                       : !ready ? "its pass is compiling"
                                : nullptr;
    const uint32_t cells = remix::trace::volumeCells(options), levels = remix::trace::volumeLevels(options);
    const float cellMeters = std::clamp(options.volumetricCellSize, .02f, 16.f);
    const float reachMeters = std::max(options.volumetricMaxDistance, cellMeters);
    remix::trace::g_volumeState = {options.volumetrics,
                                   false,
                                   v.inactiveReason ? v.inactiveReason : "",
                                   medium.source,
                                   cells,
                                   levels,
                                   medium.active ? -std::log(remix::trace::volumeVisibility) / medium.extinction *
                                                       options.sceneScale
                                                 : 0,
                                   reachMeters};
    if (v.inactiveReason)
      return;
    const float cellUnits = cellMeters / options.sceneScale;
    // Every level's cells and the one for the medium beyond them, twice: what
    // they hold, and the environment's share of it (volume.glsl).
    const size_t bytes = (size_t(cells) * cells * cells * levels + 1) * 2 * 16;
    bool reset = !v.valid || v.cellsPerSide != cells || v.levels != levels || v.cellUnits != cellUnits;
    if (!v.cells || v.bytes != bytes) {
      if (v.cells)
        frame().retiredBuffers.push_back(std::move(v.cells));
      v.cells = buffer(nullptr, bytes);
      v.bytes = bytes;
      // Every cell starts without samples.
      id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Volumetrics: clear the cells");
      [blit fillBuffer:v.cells->handle range:NSMakeRange(0, bytes) value:0];
      [blit endEncoding];
      reset = true;
    }
    v.cellsPerSide = cells;
    v.levels = levels;
    v.cellUnits = cellUnits;
    v.active = true;
    remix::trace::g_volumeState.active = true;
    constants.volumeGrid = v.cells->address;
    constants.volumeConfig[0] = 1;
    constants.volumeConfig[1] = cells;
    constants.volumeConfig[2] = levels;
    constants.volumeConfig[3] = std::clamp(options.volumetricUpdateInterval, 1u, 64u);
    constants.volumeFrame[0] = v.counter++;
    constants.volumeFrame[1] = std::clamp(options.volumetricHistory, 1u, 256u);
    constants.volumeFrame[2] = std::clamp(options.volumetricSteps, 1u, 64u);
    constants.volumeFrame[3] = reset;
    constants.volumeParams[0] = cellUnits;
    constants.volumeParams[1] = reachMeters / options.sceneScale;
    constants.volumeParams[2] = std::max(options.volumetricScattering, 0.f);
    constants.volumeParams[3] = options.volumetricBaseHeight / options.sceneScale;
    const float scales[4] = {options.volumetricSunScale, options.volumetricLightScale, options.volumetricEmissiveScale,
                             options.volumetricSkyScale};
    for (unsigned i = 0; i < 4; ++i)
      constants.volumeLightScales[i] = std::max(scales[i], 0.f);
    std::copy_n(medium.albedo, 3, constants.volumeMedium);
    constants.volumeMedium[3] = medium.extinction;
    std::fill_n(constants.volumeHeight, 4, 0.f);
    constants.volumeHeight[std::min(options.volumetricUpAxis, 2u)] = 1;
    constants.volumeHeight[3] =
        options.volumetricHeightFalloff > 0 ? options.sceneScale / options.volumetricHeightFalloff : 0;
    std::copy_n(constants.eye, 3, constants.volumeEye);
    std::copy_n(reset ? constants.eye : v.eye, 3, constants.volumePrevious);
    constants.volumeEye[3] = constants.volumePrevious[3] = 0;
    std::copy_n(constants.eye, 3, v.eye);
    v.valid = true;
  }

  // pathtrace/volume.cpp recordVolume: after the light pools are presampled
  // (the cells draw their local light from them), before any pass marching
  // the medium. One thread per cell and one for the medium beyond them.
  void PathTracer::Gpu::recordVolume(id<MTLCommandBuffer> command) {
    auto &v = volume;
    if (!v.active)
      return;
    const uint32_t count = v.cellsPerSide * v.cellsPerSide * v.cellsPerSide * v.levels;
    // The shader indexes workgroup.x * 64 + the local index (as ReGIR's
    // presampling): (count + 1) / 64 groups of 8x8, so a width of / 8.
    dispatch(command, volumeUpdate, select(volumeUpdate), "Volumetrics: light the medium's cells", (count + 1 + 7) / 8,
             1);
  }

  // pathtrace/regir.cpp configureRegir: pools are redrawn every frame from the
  // current light tree, so moving lights and rebuilt trees need no invalidation.
  void PathTracer::Gpu::configureRegir(Frame &frame, const TraceOptions &options, bool localActive) {
    auto &r = regir;
    frame.regirRead = frame.regirWrite = frame.regirSlots = 0;
    std::fill_n(frame.regirConfig, 4, 0u);
    const bool ready = regirPresample && computePipelineAsync(device, regirPresample.stage, regirPresample.base);
    r.active = options.regir && ready && localActive && lightBvh.nodes().size() > 1;
    if (r.active && !r.pool) {
      r.cells[0] = buffer(nullptr, size_t(Regir::capacity) * 16);
      r.cells[1] = buffer(nullptr, size_t(Regir::capacity) * 16);
      r.pool = buffer(nullptr, size_t(Regir::capacity) * Regir::slots * 8);
    }
    if (!r.active) {
      r.valid = false;
      return;
    }
    r.writeIndex ^= 1;
    frame.regirWrite = r.cells[r.writeIndex]->address;
    frame.regirRead = r.valid ? r.cells[r.writeIndex ^ 1]->address : 0;
    frame.regirSlots = r.pool->address;
    frame.regirConfig[0] = Regir::capacity;
    frame.regirConfig[1] = Regir::slots;
    const float scale = options.sceneScale / std::max(options.regirCellSize, .01f);
    std::memcpy(&frame.regirConfig[2], &scale, 4);
    frame.regirConfig[3] = 1;
  }

  // pathtrace/regir.cpp recordRegir: before any pass samples lights, clear
  // this frame's registration table and fill the pools of the cells
  // registered last frame.
  void PathTracer::Gpu::recordRegir(id<MTLCommandBuffer> command) {
    auto &r = regir;
    if (!r.active)
      return;
    id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"ReGIR registration clear");
    [blit fillBuffer:r.cells[r.writeIndex]->handle range:NSMakeRange(0, r.cells[r.writeIndex]->size) value:0];
    [blit endEncoding];
    // One thread per pool slot: the shader indexes workgroup.x * 64 + the
    // local index, so the grid is (capacity * slots / 64) 8x8 groups wide.
    if (r.valid)
      dispatch(command, regirPresample, select(regirPresample), "Light cell presampling (ReGIR)",
               Regir::capacity * Regir::slots / 8, 1);
    r.valid = true; // this frame registers the next frame's cells
  }

  // pathtrace/light_guiding.cpp configureLightGuiding. The guide stores
  // light-tree leaf indices and learned visibility, which do not depend on
  // light intensity: only a rebuilt tree (new leaf order) invalidates it.
  void PathTracer::Gpu::configureLightGuiding(Frame &frame, const TraceOptions &options, bool resampling) {
    auto &g = lightGuiding;
    g.stats.requested = options.localGuiding;
    g.stats.reserved = uint32_t(resampling) | (frame.localConfig[3] & 0x40000000u ? 2u : 0u);
    g.stats.influenceRange = options.localRange;
    const float cellUnits = options.localGuideCellSize / options.sceneScale;
    g.stats.cellSize = cellUnits;
    g.active = false;
    g.stats.active = 0;
    if (!options.localGuiding || frame.localConfig[0] < 2 || lightBvh.nodes().size() >= (1u << 20)) {
      g.valid = false;
      g.stats.sampledQueries = g.stats.sampledGuided = g.stats.sampledVisible = 0;
      return;
    }
    if (!g.history[0]) {
      g.history[0] = buffer(nullptr, (LightGuiding::capacity + 1) * 16);
      g.history[1] = buffer(nullptr, (LightGuiding::capacity + 1) * 16);
      g.stats.capacity = LightGuiding::capacity;
      g.stats.memoryBytes = g.history[0]->size + g.history[1]->size;
    }
    if (history.reset() || g.generation != lightBvh.builds || g.range != options.localRange ||
        g.cellSize != cellUnits || g.resampled != resampling) {
      g.valid = false;
      ++g.stats.historyResets;
    }
    g.generation = lightBvh.builds;
    g.range = options.localRange;
    g.cellSize = cellUnits;
    g.resampled = resampling;
    frame.lightGuideRead = g.valid ? g.history[g.writeIndex ^ 1]->address : 0;
    frame.lightGuideWrite = g.history[g.writeIndex]->address;
    frame.lightGuideConfig[0] = LightGuiding::capacity;
    frame.lightGuideConfig[1] = uint32_t(lightBvh.nodes().size());
    const float scale = 1.f / cellUnits;
    std::memcpy(&frame.lightGuideConfig[2], &scale, 4);
    g.stats.active = 1;
    g.active = true;
  }

  // The read bank is immutable for the whole frame: clear the bank this frame
  // trains before any pass's atomics.
  void PathTracer::Gpu::beginLightGuiding(id<MTLCommandBuffer> command) {
    if (!lightGuiding.active)
      return;
    const auto &bank = lightGuiding.history[lightGuiding.writeIndex];
    id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Light guiding clear");
    [blit fillBuffer:bank->handle range:NSMakeRange(0, bank->size) value:0];
    [blit endEncoding];
  }

  // pathtrace/sky_guiding.cpp configureSkyGuiding. Only the extra sky rays of a
  // path's first surface sample the learned directions, so it is active with
  // skySamples above one, a sky, and the denoiser (a progressive reference
  // keeps plain BRDF sampling).
  void PathTracer::Gpu::configureSkyGuiding(Frame &frame, const TraceOptions &options) {
    auto &g = skyGuiding;
    g.active = false;
    frame.skyGuideRead = frame.skyGuideWrite = 0;
    std::fill_n(frame.skyGuideConfig, 4, 0u);
    if (!options.skyGuiding || options.skySamples < 2 || frame.skyRadiance[3] == 0 || !options.sky ||
        !denoiser || options.accumulate)
      return;
    if (!g.history[0]) {
      const size_t bytes = size_t(SkyGuiding::capacity) * SkyGuiding::stride * 4;
      g.history[0] = buffer(nullptr, bytes);
      g.history[1] = buffer(nullptr, bytes);
    }
    const float cellUnits = options.skyGuideCellSize / options.sceneScale;
    if (history.reset() || g.cellSize != cellUnits)
      g.valid = false;
    g.cellSize = cellUnits;
    frame.skyGuideRead = g.valid ? g.history[g.writeIndex ^ 1]->address : 0;
    frame.skyGuideWrite = g.history[g.writeIndex]->address;
    const float scale = 1.f / cellUnits, share = std::clamp(options.skyGuideShare, 0.f, .95f);
    frame.skyGuideConfig[0] = SkyGuiding::capacity;
    std::memcpy(&frame.skyGuideConfig[1], &scale, 4);
    std::memcpy(&frame.skyGuideConfig[2], &share, 4);
    frame.skyGuideConfig[3] = 1;
    g.active = true;
  }

  // As beginLightGuiding: the read bank stays immutable for the frame; clear
  // the bank this frame trains before any pass's atomics.
  void PathTracer::Gpu::beginSkyGuiding(id<MTLCommandBuffer> command) {
    if (!skyGuiding.active)
      return;
    const auto &bank = skyGuiding.history[skyGuiding.writeIndex];
    id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Sky guiding clear");
    [blit fillBuffer:bank->handle range:NSMakeRange(0, bank->size) value:0];
    [blit endEncoding];
  }

  // The trained bank's counters (after its last entry) for statistics, read
  // once this frame slot has completed (readLightGuiding).
  void PathTracer::Gpu::finishLightGuiding(id<MTLCommandBuffer> command) {
    if (!lightGuiding.active)
      return;
    auto &f = frame();
    if (!f.guideCounters)
      f.guideCounters = metal::buffer(device, nullptr, 16);
    id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Light guiding counters");
    [blit copyFromBuffer:lightGuiding.history[lightGuiding.writeIndex]->handle
            sourceOffset:size_t(LightGuiding::capacity) * 16
                toBuffer:f.guideCounters
       destinationOffset:0
                    size:16];
    [blit endEncoding];
    f.guidePending = true;
  }

  void PathTracer::Gpu::readLightGuiding() {
    auto &f = frame();
    if (!f.guidePending)
      return;
    uint32_t counters[4];
    std::memcpy(counters, f.guideCounters.contents, sizeof(counters));
    lightGuiding.stats.sampledQueries = counters[0];
    lightGuiding.stats.sampledGuided = counters[1];
    lightGuiding.stats.sampledVisible = counters[2];
    f.guidePending = false;
  }

  void PathTracer::Gpu::beginRayProfile(id<MTLCommandBuffer> command, Frame &) {
    if (!rayProfile.counters)
      return;
    id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Ray profile clear");
    [blit fillBuffer:rayProfile.counters->handle range:NSMakeRange(0, 768) value:0];
    [blit endEncoding];
  }

  void PathTracer::Gpu::finishRayProfile(id<MTLCommandBuffer> command) {
    if (!rayProfile.counters)
      return;
    auto &f = frame();
    if (!f.rayCounts)
      f.rayCounts = metal::buffer(device, nullptr, 768);
    id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Ray profile counters");
    [blit copyFromBuffer:rayProfile.counters->handle sourceOffset:0 toBuffer:f.rayCounts destinationOffset:0 size:768];
    [blit endEncoding];
    f.rayPending = true;
  }

  // pathtrace/ray_profile.cpp completeRayProfile: the same CSV columns.
  void PathTracer::Gpu::readRayProfile() {
    auto &f = frame();
    if (!f.rayPending)
      return;
    f.rayPending = false;
    uint32_t counts[192] = {};
    std::memcpy(counts, f.rayCounts.contents, sizeof(counts));
    auto *file = std::fopen(rayProfile.path, "a");
    if (!file)
      return;
    if (std::ftell(file) == 0) {
      std::fprintf(file, "time_ms");
      for (const char *pass : {"opaque", "direct", "transparency"})
        for (const char *ray : {"scene", "sun", "local"})
          for (const char *event :
               {"queries", "candidates", "alpha", "alpha_reject", "back_reject", "committed", "alpha_texture", "reserved"})
            std::fprintf(file, ",%s_%s_%s", pass, ray, event);
      for (const char *pass : {"opaque", "direct", "transparency"})
        for (const char *event : {"starts", "depth1", "depth2", "depth3", "depth4", "miss", "zero", "cache", "limit",
                                  "sheet_limit", "roulette_tests", "roulette_survivals", "roulette_terminated"})
          std::fprintf(file, ",%s_path_%s", pass, event);
      std::fprintf(file, "\n");
    }
    const auto time =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(file, "%lld", static_cast<long long>(time));
    for (unsigned pass = 0; pass < 3; ++pass)
      for (unsigned i = 0; i < 24; ++i)
        std::fprintf(file, ",%u", counts[pass * 64 + i]);
    for (unsigned pass = 0; pass < 3; ++pass)
      for (unsigned i = 24; i < 37; ++i)
        std::fprintf(file, ",%u", counts[pass * 64 + i]);
    std::fprintf(file, "\n");
    std::fclose(file);
  }

  PathTracer::PostStatistics PathTracer::postStatistics() const {
    if (!m_gpu->post)
      return {};
    const auto s = m_gpu->post->statistics();
    return {s.exposure, s.target, s.limit, s.milliseconds, s.memoryBytes};
  }

  remixapi_LocalLightGuidingStatisticsEXT PathTracer::localLightGuidingStatistics() const {
    return m_gpu->lightGuiding.stats;
  }

  bool PathTracer::translucencyDenoiserActive() const {
    return m_gpu->denoiser && m_gpu->denoiser->foregroundActive();
  }
  ReconstructionStatistics PathTracer::upscalingStatistics() const {
    const auto &g = *m_gpu;
    const auto &c = g.configuration;
    ReconstructionStatistics result;
    result.backend = c.reconstruction == uint32_t(reconstruction::Backend::MetalFXDenoising)
                         ? ReconstructionBackend::MetalFxDenoising
                     : c.reconstruction ? ReconstructionBackend::MetalFxUpscaling
                                        : ReconstructionBackend::Native;
    result.active = c.reconstruction != 0;
    result.render = {g.width, g.height};
    result.display = {c.displayWidth, c.displayHeight};
    result.failure = c.reconstructionUnavailable;
    if (g.reconstruction && c.reconstruction) {
      const auto s = g.reconstruction->statistics();
      result.frames = s.frames;
      result.resets = s.resets;
      result.memoryBytes = s.memoryBytes;
    }
    return result;
  }
  void *PathTracer::interpolationDepth() const {
    return m_gpu->reconstruction ? (__bridge void *)m_gpu->reconstruction->deviceDepth() : nullptr;
  }
  void *PathTracer::interpolationMotion() const {
    return m_gpu->reconstruction ? (__bridge void *)m_gpu->reconstruction->motion() : nullptr;
  }
  void *PathTracer::interpolationScaler() const {
    return m_gpu->reconstruction ? (__bridge void *)m_gpu->reconstruction->scaler() : nullptr;
  }
  void PathTracer::interpolationCamera(scene::Matrix &projection, scene::Matrix &view, float *jitter,
                                       bool &reset) const {
    const auto &g = *m_gpu;
    projection = g.pendingProjection;
    view = g.temporalFrame.view;
    jitter[0] = g.temporalFrame.jitter[0];
    jitter[1] = g.temporalFrame.jitter[1];
    reset = g.temporalFrame.flags[0] != 0;
  }
  // pathtrace/temporal.cpp localLightStatistics.
  remixapi_LocalLightStatisticsEXT PathTracer::localLightStatistics() const {
    const auto &g = *m_gpu;
    remixapi_LocalLightStatisticsEXT result{.structSize = sizeof(result)};
    result.lightCount = uint32_t(g.lightBvh.emitters().size());
    result.activeLights = g.lightBvh.active;
    result.nodeCount = uint32_t(g.lightBvh.nodes().size());
    result.treeDepth = g.lightBvh.depth;
    result.generation = g.lightBvh.generation;
    return result;
  }
  void PathTracer::resetPerformance() {
    m_gpu->performance = {.structSize = sizeof(remixapi_FramePerformanceEXT)};
  }
} // namespace remix::metal
