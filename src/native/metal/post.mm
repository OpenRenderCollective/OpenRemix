// Automatic exposure and bloom (postfx/renderer.cpp's counterpart): the same
// passes and the same exposure state (postfx/meter.h), recorded with Metal.
#include "metal/post.h"
#include "postfx/meter.h"
#include <cstring>

namespace remix::metal::post {
  namespace {
    using shaders::Stage;
    // postfx.comp's push constants (postfx/renderer.cpp Push).
    struct Push {
      uint32_t width{}, height{}, groups{};
      float centre{};
      float target{}, blend{}, minEv{}, maxEv{}, radius{};
      uint32_t reset{};
    };
    // postfx.comp set 0: two sampled sources, the output image, the meter's
    // partial sums and the exposure state.
    constexpr uint32_t bindingSource = 0, bindingSecond = 1, bindingOutput = 2, bindingPartials = 3,
                       bindingExposure = 4;
  } // namespace

  struct Renderer::Gpu {
    id<MTLDevice> device = nil;
    uint32_t displayWidth = 0, displayHeight = 0;
    id<MTLTexture> guide = nil, dummy = nil;
    std::vector<id<MTLTexture>> down, up;
    id<MTLBuffer> partials = nil, exposure = nil;
    id<MTLBuffer> settings[gpuFrameCount] = {}, readback[gpuFrameCount] = {};
    bool pending[gpuFrameCount] = {};
    id<MTLComputePipelineState> pipelines[4] = {};
    // The meter and the resolve bind the same resources, but SPIRV-Cross lays
    // out each stage's argument buffer by the resources it uses: one set each.
    ArgumentSet meterSet, resolveSet;
    std::vector<ArgumentSet> downSets, upSets;
    id<MTLSamplerState> edge = nil, border = nil;
    uint32_t slot = 0;
    bool initialized = false, bloomValid = false, activeMeter = false;
    // postfx::State: survives nothing on Metal (one renderer per tracer).
    bool enabled = false, epochValid = false;
    scene::TemporalEpoch epoch{};
    std::chrono::steady_clock::time_point time{};
    float previousRadius = -1;
    std::array<float, 5> previousMeter{};
    unsigned settlingFrames = 0;
    uint64_t sourceGeneration = UINT64_MAX;
    Statistics stats;

    void dispatch(id<MTLCommandBuffer> command, uint32_t pass, ArgumentSet &set, const Push &push,
                  const char *label) {
      static constexpr Stage stages[] = {Stage::postMeter, Stage::postResolve, Stage::postDown, Stage::postUp};
      const auto &source = library(stages[pass]);
      id<MTLComputeCommandEncoder> encoder = computeEncoder(command, @(label), push.width, push.height);
      [encoder setComputePipelineState:pipelines[pass]];
      [encoder setBuffer:set.current() offset:0 atIndex:0];
      set.use(encoder);
      [encoder setBytes:&push length:sizeof(push) atIndex:source.push];
      // postfx/renderer.cpp dispatch: 16x16 meter tiles, one resolve group,
      // 8x8 tiles for the bloom chain.
      const unsigned tile = pass == 0 ? 16 : 8;
      const MTLSize groups = pass == 1 ? MTLSizeMake(1, 1, 1)
                                       : MTLSizeMake((push.width + tile - 1) / tile, (push.height + tile - 1) / tile, 1);
      [encoder dispatchThreadgroups:groups
              threadsPerThreadgroup:MTLSizeMake(source.threads[0], source.threads[1], source.threads[2])];
      [encoder endEncoding];
    }
    void write(ArgumentSet &set, id<MTLTexture> source, id<MTLTexture> second, id<MTLTexture> output,
               id<MTLSamplerState> sampler) {
      set.begin(slot);
      set.texture(bindingSource, source, sampler);
      set.texture(bindingSecond, second, edge);
      set.texture(bindingOutput, output);
      set.buffer(bindingPartials, partials);
      set.buffer(bindingExposure, exposure);
    }
  };

  Renderer::Renderer() = default;
  Renderer::~Renderer() = default;

  void Renderer::initialize(id<MTLDevice> device, uint32_t displayWidth, uint32_t displayHeight,
                            uint32_t renderWidth, uint32_t renderHeight) {
    m_gpu = std::make_unique<Gpu>();
    auto &g = *m_gpu;
    g.device = device;
    g.displayWidth = displayWidth;
    g.displayHeight = displayHeight;
    const auto usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    g.dummy = texture2d(device, MTLPixelFormatRGBA16Float, 1, 1, usage | MTLTextureUsageRenderTarget);
    g.guide = texture2d(device, MTLPixelFormatRGBA16Float, renderWidth, renderHeight, usage);
    for (auto size : postfx::pyramid({displayWidth, displayHeight})) {
      g.down.push_back(texture2d(device, MTLPixelFormatRGBA16Float, size.width, size.height, usage));
      if (g.down.size() > 1) {
        id<MTLTexture> previous = g.down[g.down.size() - 2];
        g.up.push_back(texture2d(device, MTLPixelFormatRGBA16Float, uint32_t(previous.width),
                                 uint32_t(previous.height), usage));
      }
    }
    const auto groups = ((displayWidth + 15) / 16) * ((displayHeight + 15) / 16);
    g.partials = buffer(device, nullptr, size_t(groups) * 8);
    // The exposure state starts at unity (postfx Renderer::begin).
    const postfx::Exposure unity;
    g.exposure = buffer(device, &unity, sizeof(unity));
    for (unsigned i = 0; i < gpuFrameCount; ++i) {
      g.settings[i] = buffer(device, nullptr, 16);
      g.readback[i] = buffer(device, nullptr, 16);
    }
    g.edge = sampler(device, 1, 3, 3);
    g.border = sampler(device, 1, 4, 4);
    const Stage stages[] = {Stage::postMeter, Stage::postResolve, Stage::postDown, Stage::postUp};
    for (unsigned i = 0; i < 4; ++i)
      g.pipelines[i] = computePipeline(device, stages[i]);
    g.meterSet = ArgumentSet(device, Stage::postMeter, 0);
    g.resolveSet = ArgumentSet(device, Stage::postResolve, 0);
    for (size_t i = 0; i < g.down.size(); ++i)
      g.downSets.emplace_back(device, Stage::postDown, 0);
    for (size_t i = 0; i < g.up.size(); ++i)
      g.upSets.emplace_back(device, Stage::postUp, 0);
    g.stats.memoryBytes = g.guide.allocatedSize + g.partials.allocatedSize + g.exposure.allocatedSize;
    for (id<MTLTexture> t : g.down)
      g.stats.memoryBytes += t.allocatedSize;
    for (id<MTLTexture> t : g.up)
      g.stats.memoryBytes += t.allocatedSize;
  }

  void Renderer::selectFrame(uint32_t slot) {
    auto &g = *m_gpu;
    g.slot = slot % gpuFrameCount;
    if (!g.pending[g.slot])
      return;
    postfx::Exposure value;
    std::memcpy(&value, g.readback[g.slot].contents, sizeof(value));
    g.stats.exposure = value.current;
    g.stats.target = value.target;
    g.stats.limit = value.limit;
    g.stats.adapting =
        g.activeMeter && std::abs(value.current - value.target) > std::max(value.target, 1e-6f) * .001f;
    g.pending[g.slot] = false;
  }

  id<MTLTexture> Renderer::guide() const { return m_gpu->guide; }

  void Renderer::record(id<MTLCommandBuffer> command, id<MTLTexture> hdr, const TraceOptions &options,
                        scene::TemporalEpoch epoch, bool changed, bool bypass, uint64_t generation) {
    auto &g = *m_gpu;
    if (!g.initialized) {
      // History starts from zero, as vkCmdClearColorImage of the dummy.
      auto *clear = [MTLRenderPassDescriptor renderPassDescriptor];
      clear.colorAttachments[0].texture = g.dummy;
      clear.colorAttachments[0].loadAction = MTLLoadActionClear;
      clear.colorAttachments[0].storeAction = MTLStoreActionStore;
      clear.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
      [[command renderCommandEncoderWithDescriptor:clear] endEncoding];
      g.initialized = true;
    }
    if (changed)
      g.bloomValid = false;
    const bool meter = !bypass && options.autoExposure;
    const bool bloom = !bypass && options.bloom && !g.down.empty() && options.bloomIntensity > 0;
    g.activeMeter = meter;
    if (!meter)
      g.stats.adapting = false;
    const auto now = std::chrono::steady_clock::now();
    const float dt =
        g.time == std::chrono::steady_clock::time_point{} ? 0.f : std::chrono::duration<float>(now - g.time).count();
    g.time = meter ? now : std::chrono::steady_clock::time_point{};
    const bool reset = !g.enabled || !g.epochValid || g.epoch != epoch;
    if (!bypass) {
      g.enabled = options.autoExposure;
      g.epoch = epoch;
      g.epochValid = true;
    }
    if (meter) {
      const std::array<float, 5> meterSettings = {options.autoExposureTarget, options.autoExposureCentre,
                                                  options.autoExposureMinEv, options.autoExposureMaxEv,
                                                  options.autoExposureSpeed};
      if (meterSettings != g.previousMeter || reset || generation != g.sourceGeneration)
        g.settlingFrames = gpuFrameCount;
      g.previousMeter = meterSettings;
      g.sourceGeneration = generation;
      g.write(g.meterSet, hdr, g.guide, g.dummy, g.edge);
      g.write(g.resolveSet, hdr, g.guide, g.dummy, g.edge);
      const Push push{g.displayWidth,
                      g.displayHeight,
                      ((g.displayWidth + 15) / 16) * ((g.displayHeight + 15) / 16),
                      options.autoExposureCentre,
                      options.autoExposureTarget,
                      postfx::adaptation(dt, options.autoExposureSpeed),
                      options.autoExposureMinEv,
                      options.autoExposureMaxEv,
                      0,
                      uint32_t(reset)};
      g.dispatch(command, 0, g.meterSet, push, "Auto exposure meter");
      g.dispatch(command, 1, g.resolveSet, push, "Auto exposure resolve");
      ++g.stats.meterFrames;
      if (g.settlingFrames) {
        --g.settlingFrames;
        g.stats.adapting = true;
      }
    }
    if (bloom && (!g.bloomValid || g.previousRadius != options.bloomRadius)) {
      for (size_t i = 0; i < g.down.size(); ++i) {
        id<MTLTexture> target = g.down[i];
        g.write(g.downSets[i], i ? g.down[i - 1] : hdr, g.dummy, target, g.border);
        g.dispatch(command, 2, g.downSets[i], {uint32_t(target.width), uint32_t(target.height)},
                   "Bloom downsample");
      }
      for (int i = int(g.up.size()) - 1; i >= 0; --i) {
        id<MTLTexture> target = g.up[i];
        g.write(g.upSets[i], i + 1 == int(g.up.size()) ? g.down.back() : g.up[i + 1], g.down[i], target, g.edge);
        Push push{uint32_t(target.width), uint32_t(target.height)};
        push.radius = options.bloomRadius;
        g.dispatch(command, 3, g.upSets[i], push, "Bloom upsample");
      }
      g.previousRadius = options.bloomRadius;
      g.bloomValid = true;
      ++g.stats.bloomFrames;
    }
    const float values[] = {bypass ? 1.f : options.exposure, bloom ? options.bloomIntensity : 0.f,
                            float(!bypass && options.autoExposure),
                            !bypass && options.contrast ? options.contrastAmount : 0.f};
    std::memcpy(g.settings[g.slot].contents, values, sizeof(values));
    // The exposure the frame used, for statistics once it completes.
    id<MTLBlitCommandEncoder> blit = blitEncoder(command, @"Exposure readback");
    [blit copyFromBuffer:g.exposure sourceOffset:0 toBuffer:g.readback[g.slot] destinationOffset:0 size:16];
    [blit endEncoding];
    g.pending[g.slot] = true;
  }

  id<MTLBuffer> Renderer::exposure() const { return m_gpu->exposure; }
  id<MTLTexture> Renderer::bloom() const {
    const auto &g = *m_gpu;
    return !g.up.empty() ? g.up[0] : !g.down.empty() ? g.down[0] : g.dummy;
  }
  id<MTLBuffer> Renderer::settings() const { return m_gpu->settings[m_gpu->slot]; }
  Statistics Renderer::statistics() const { return m_gpu->stats; }
} // namespace remix::metal::post
