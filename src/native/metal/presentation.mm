// Presentation (runtime/presentation.cpp's counterpart): one command buffer
// per frame records uploads, tracing, the display pass and the UI, presents
// the drawable and signals the frame slot when the GPU completes it.
#import <MetalFX/MetalFX.h>
#include "metal/runtime_gpu.h"
#include "platform/utf8_path.h"
#include "ui/context.h"
#include "upscale/camera.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <thread>
#include <fstream>
#include <imgui.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <image/stb_image_write.h>

namespace remix {
  namespace {
    using Clock = std::chrono::steady_clock;
    double since(Clock::time_point start) {
      return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    // REMIX_GPU_PASS_LOG: accumulates frames' pass times and logs their means
    // every `frames` frames, the slowest first.
    void logPassTimings(const gpu::FrameTimings &timings, uint32_t frames) {
      static std::vector<std::pair<std::string, double>> sums;
      static double frameSum = 0;
      static uint32_t count = 0;
      frameSum += timings.frameMilliseconds;
      for (const auto &pass : timings.passes) {
        auto found = std::find_if(sums.begin(), sums.end(), [&](const auto &s) { return s.first == pass.name; });
        if (found == sums.end())
          sums.emplace_back(pass.name, pass.selfMilliseconds);
        else
          found->second += pass.selfMilliseconds;
      }
      if (++count < frames)
        return;
      auto sorted = sums;
      std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
      std::string line;
      for (const auto &[name, sum] : sorted)
        if (sum / count >= .05)
          line += "\n  " + std::to_string(sum / count).substr(0, 5) + " ms  " + name;
      SDL_Log("OpenRemix GPU passes (%u frames, %ux%u -> %ux%u): frame %.2f ms%s", count, timings.renderWidth,
              timings.renderHeight, timings.outputWidth, timings.outputHeight, frameSum / count, line.c_str());
      sums.clear();
      frameSum = 0;
      count = 0;
    }

    // Creates the frame interpolator for the tracer's configuration at the
    // drawable size; false when frame generation is off or unavailable.
    bool prepareInterpolation(RemixNativeRuntime::Gpu &g, const metal::PathTracer &tracer) {
      auto &f = g.interpolation;
      const auto &c = tracer.configuration();
      void *depth = tracer.interpolationDepth();
      const auto release = [&] {
        f.interpolator = nil;
        for (auto &slot : f.slots)
          slot = {};
        f.previous = UINT32_MAX;
      };
      if (!c.frameGeneration || !depth || c.displayWidth != g.width || c.displayHeight != g.height) {
        release();
        f.depth = nullptr;
        f.failure.clear();
        return false;
      }
      if (f.depth == depth && f.width == g.width && f.height == g.height)
        return f.interpolator != nil;
      release();
      f.depth = depth;
      f.width = g.width;
      f.height = g.height;
      f.inputWidth = c.width;
      f.inputHeight = c.height;
      f.reset = true;
      f.failure.clear();
      if (@available(macOS 26.0, *)) {
        if (![MTLFXFrameInterpolatorDescriptor supportsDevice:g.device])
          f.failure = "MetalFX frame interpolation is not supported on this device";
        else {
          auto *descriptor = [MTLFXFrameInterpolatorDescriptor new];
          descriptor.colorTextureFormat = g.format;
          descriptor.outputTextureFormat = g.format;
          descriptor.depthTextureFormat = MTLPixelFormatR32Float;
          descriptor.motionTextureFormat = MTLPixelFormatRG32Float;
          descriptor.uiTextureFormat = g.format;
          descriptor.inputWidth = c.width;
          descriptor.inputHeight = c.height;
          descriptor.outputWidth = g.width;
          descriptor.outputHeight = g.height;
          id<MTLFXFrameInterpolator> interpolator = [descriptor newFrameInterpolatorWithDevice:g.device];
          if (!interpolator)
            f.failure = "MetalFX could not create a frame interpolator";
          else {
            f.interpolator = interpolator;
            for (auto &slot : f.slots) {
              slot.color = metal::texture2d(g.device, g.format, g.width, g.height,
                                            interpolator.colorTextureUsage | MTLTextureUsageRenderTarget |
                                                MTLTextureUsageShaderRead);
              slot.output = metal::texture2d(g.device, g.format, g.width, g.height,
                                             interpolator.outputTextureUsage | MTLTextureUsageRenderTarget |
                                                 MTLTextureUsageShaderRead);
              slot.real = metal::texture2d(g.device, g.format, g.width, g.height,
                                           MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
            }
            if (!f.presentQueue) {
              f.presentQueue = [g.device newCommandQueue];
              f.presentQueue.label = @"OpenRemix present";
              f.presentDispatch = dispatch_queue_create("OpenRemix present", DISPATCH_QUEUE_SERIAL);
            }
          }
        }
      } else
        f.failure = "MetalFX frame interpolation requires macOS 26";
      if (!f.failure.empty())
        SDL_Log("OpenRemix frame generation off: %s", f.failure.c_str());
      return f.interpolator != nil;
    }

    // Encodes the frame between the previous slot's color and this one's into
    // its output; false on a history reset (nothing to interpolate from).
    bool interpolate(RemixNativeRuntime::Gpu &g, const metal::PathTracer &tracer, id<MTLCommandBuffer> command) {
      auto &f = g.interpolation;
      const auto &slot = f.slots[g.frameIndex];
      const auto now = Clock::now();
      float delta = f.last == Clock::time_point{} ? 1.f / 60 : std::chrono::duration<float>(now - f.last).count();
      f.last = now;
      scene::Matrix projection, view;
      float jitter[2];
      bool reset = false;
      tracer.interpolationCamera(projection, view, jitter, reset);
      reset = reset || f.reset || f.previous >= gpuFrameCount || f.previous == g.frameIndex;
      f.reset = false;
      if (@available(macOS 26.0, *)) {
        id<MTLFXFrameInterpolator> interpolator = f.interpolator;
        interpolator.colorTexture = slot.color;
        interpolator.prevColorTexture = f.slots[reset ? g.frameIndex : f.previous].color;
        interpolator.depthTexture = (__bridge id<MTLTexture>)tracer.interpolationDepth();
        interpolator.motionTexture = (__bridge id<MTLTexture>)tracer.interpolationMotion();
        interpolator.outputTexture = slot.output;
        // Pixel motion toward the previous frame in render pixels, reversed
        // device depth and FSR's jitter sign: the temporal scaler's inputs.
        interpolator.motionVectorScaleX = 1;
        interpolator.motionVectorScaleY = 1;
        interpolator.depthReversed = YES;
        interpolator.jitterOffsetX = -jitter[0];
        interpolator.jitterOffsetY = -jitter[1];
        f.interval = std::clamp(delta, 1.f / 480, .1f);
        interpolator.deltaTime = f.interval;
        const auto range = upscale::depthRange(projection);
        interpolator.nearPlane = range.nearPlane;
        interpolator.farPlane = std::isfinite(range.farPlane) ? range.farPlane : 1e6f;
        const float focal = std::abs(projection[5]);
        interpolator.fieldOfView = focal > 0 ? 2 * std::atan(1 / focal) * 57.29577951f : 60;
        interpolator.aspectRatio = projection[0] != 0 ? std::abs(projection[5] / projection[0])
                                                      : float(g.width) / float(g.height);
        interpolator.shouldResetHistory = reset;
        if (!reset) {
          [interpolator encodeToCommandBuffer:command];
          metal::markTimed(command, @"MetalFX frame interpolation");
        }
      }
      return !reset;
    }
  } // namespace

  // runtime/screenshot.cpp PresentedScreenshot::request: one capture at a time.
  remixapi_ErrorCode RemixNativeRuntime::requestScreenshot(const char *path) {
    if (!m_initialized)
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    const auto destination = utf8Path(path);
    if (!destination.is_absolute())
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (destination.extension() != ".png")
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    if (!m_screenshotPath.empty() || m_gpu->screenshot.command)
      return REMIXAPI_ERROR_CODE_ALREADY_EXISTS;
    m_screenshotPath = path;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // Writes a capture once its frame completed: without waiting from
  // PumpEvents, waiting before the next presentation records.
  void RemixNativeRuntime::completeScreenshot() { completeScreenshot(false); }

  void RemixNativeRuntime::completeScreenshot(bool wait) {
    if (!m_gpu)
      return;
    auto &s = m_gpu->screenshot;
    if (!s.command)
      return;
    if (wait)
      [s.command waitUntilCompleted];
    else if (s.command.status < MTLCommandBufferStatusCompleted)
      return;
    s.command = nil;
    std::vector<uint8_t> pixels(size_t(s.width) * s.height * 4);
    std::memcpy(pixels.data(), s.readback.contents, pixels.size());
    for (size_t i = 0; i < pixels.size(); i += 4) {
      if (s.bgra)
        std::swap(pixels[i], pixels[i + 2]);
      pixels[i + 3] = 255;
    }
    std::ofstream output(utf8Path(s.path.c_str()), std::ios::binary);
    const auto writer = [](void *context, void *data, int size) {
      static_cast<std::ofstream *>(context)->write(static_cast<const char *>(data), size);
    };
    const int written =
        output && stbi_write_png_to_func(writer, &output, int(s.width), int(s.height), 4, pixels.data(), int(s.width * 4));
    output.flush();
    if (!written || !output)
      SDL_Log("Screenshot write failed: %s", s.path.c_str());
    s.readback = nil;
    s.path.clear();
  }

  remixapi_ErrorCode RemixNativeRuntime::present(const remixapi_PresentInfo *info) {
    if (info && info->sType != REMIXAPI_STRUCT_TYPE_PRESENT_INFO)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (!isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    if (m_failed)
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    // The frame's draws are released whatever happens (raster.finishFrame).
    struct EndFrame {
      metal::Raster &raster;
      ~EndFrame() { raster.finishFrame(); }
    } endFrame{m_raster};
    @autoreleasepool {
      try {
        return presentFrame();
      } catch (const std::exception &error) {
        return fail(error.what());
      } catch (...) {
        return fail("unknown error");
      }
    }
  }

  remixapi_ErrorCode RemixNativeRuntime::presentFrame() {
    auto &g = *m_gpu;
    auto &p = m_framePerformance;
    m_previousPerformance = m_framePerformance;
    m_previousBreakdown = m_frameBreakdown;
    p = {.structSize = sizeof(p), .attempts = p.attempts + 1, .presentations = m_presentationCount};
    m_frameBreakdown = {.structSize = sizeof(m_frameBreakdown)};
    const auto start = Clock::now();
    // As runtime/presentation.cpp CompletePerformance: presented is whether
    // this call presented.
    struct PresentTime {
      remixapi_FramePerformanceEXT &performance;
      const uint64_t &presentations;
      Clock::time_point start;
      ~PresentTime() {
        performance.presentMilliseconds = since(start);
        performance.presented = presentations != performance.presentations;
        performance.presentations = presentations;
      }
    } presentTime{p, m_presentationCount, start};
    if (m_pathTracer)
      m_pathTracer->resetPerformance();
    remixapi_WindowState state = {};
    auto status = m_window.getWindowState(state);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS)
      return status;
    if (g.commandFailed)
      return fail("a command buffer failed");
    applySettings();
    if (state.minimized || !state.drawableWidth || !state.drawableHeight)
      return REMIXAPI_ERROR_CODE_SUCCESS;
    if (state.drawableWidth != g.width || state.drawableHeight != g.height) {
      g.width = state.drawableWidth;
      g.height = state.drawableHeight;
      g.layer.drawableSize = CGSizeMake(g.width, g.height);
    }
    g.layer.displaySyncEnabled = m_vsync;
    m_raster.gpu().configureTarget(g.format, g.width, g.height, true);
    updateFreeCamera();
    m_raster.applyFreeCamera(m_freeCamera);
    prepareSettings(g.width, g.height);
    const auto &settings = m_profiles.current();
    const bool tracing = m_pathTracer && m_backend;
    if (!tracing)
      m_raster.expandStatic();
    m_raster.setPersistentStatic(settings.persistentStatic && tracing);
    // CPU scene preparation overlaps the frames still on the GPU.
    std::optional<scene::TraceFrame> traceScene;
    if (tracing) {
      const auto sceneStart = Clock::now();
      traceScene = m_pathTracer->prepareScene(m_raster, m_traceOptions);
      p.sceneMilliseconds = since(sceneStart);
    }
    {
      const auto fenceStart = Clock::now();
      if (dispatch_semaphore_wait(g.frames, dispatch_time(DISPATCH_TIME_NOW, int64_t(1e9))))
        return REMIXAPI_ERROR_CODE_SUCCESS; // A stalled GPU skips the frame.
      p.fenceMilliseconds = since(fenceStart);
    }
    // From here the slot is ours: release it on every path.
    struct Slot {
      dispatch_semaphore_t frames;
      bool committed = false;
      ~Slot() {
        if (!committed)
          dispatch_semaphore_signal(frames);
      }
    } slot{g.frames};
    g.frameIndex = (g.frameIndex + 1) % g.frameCount;
    m_raster.selectFrame(g.frameIndex);
    // GPU pass timers: this slot's previous frame completed, its timings are
    // final; then this frame's encoders are timed when the setting is on.
    // REMIX_GPU_PASS_LOG=N: every N timed frames, the passes' mean GPU times
    // (rtx.native.gpuPassTimers for hosts without the developer panel).
    static const uint32_t passLog = [] {
      const char *value = std::getenv("REMIX_GPU_PASS_LOG");
      return value ? uint32_t(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    g.timers.beginFrame(g.frameIndex, m_profiles.current().gpuPassTimers || passLog, m_presentationCount);
    if (passLog && g.timers.latest().available && g.timers.latest().frame != m_gpuTimings.frame)
      logPassTimings(g.timers.latest(), passLog);
    m_gpuTimings = g.timers.latest();
    if (m_pathTracer && m_backend) {
      const auto &c = m_pathTracer->configuration();
      g.timers.setSizes(c.width, c.height, c.displayWidth, c.displayHeight);
    } else
      g.timers.setSizes(g.width, g.height, g.width, g.height);
    struct EndTimers {
      metal::PassTimers &timers;
      ~EndTimers() { timers.endFrame(); }
    } endTimers{g.timers};
    if (m_pathTracer)
      m_pathTracer->selectFrame(g.frameIndex);
    completeScreenshot(true);
    const auto recordStart = Clock::now();
    id<MTLCommandBuffer> command = [g.queue commandBuffer];
    command.label = @"OpenRemix frame";
    m_raster.prepare(tracing);
    auto &raster = m_raster.gpu();
    if (tracing) {
      // Role CUT (remixapi_InstanceInfoMaskEXT): the coverage that hides
      // traced instances from the camera, ahead of primary visibility.
      if (raster.mask.cut.count)
        raster.recordMasks(command, true);
      m_pathTracer->prepare((__bridge void *)command, m_raster, m_traceOptions, *traceScene);
    }
    buildUi();
    const bool interpolating = tracing && prepareInterpolation(g, *m_pathTracer);
    // Frame generation acquires its drawables on the present queue.
    const auto acquireStart = Clock::now();
    id<CAMetalDrawable> drawable = interpolating ? nil : [g.layer nextDrawable];
    p.acquireMilliseconds = since(acquireStart);
    if (!interpolating && !drawable) {
      [command commit];
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    m_frameStarted = true;
    // With frame generation the scene renders offscreen for the interpolator
    // and the UI is drawn over each presented frame afterwards.
    auto &frameSlot = g.interpolation.slots[g.frameIndex];
    id<MTLTexture> scene = interpolating ? frameSlot.color : drawable.texture;
    auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = scene;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
    pass.depthAttachment.texture = raster.depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    // Passes that resume the frame (overlay coverage, halos, soft sprites)
    // keep testing its depth.
    pass.depthAttachment.storeAction = raster.splitsFrame() ? MTLStoreActionStore : MTLStoreActionDontCare;
    pass.depthAttachment.clearDepth = 1;
    id<MTLRenderCommandEncoder> encoder =
        metal::renderEncoder(command, pass, tracing ? @"Composite, tonemap and raster overlay" : @"Raster scene",
                             g.width, g.height);
    // The frame's pass again, after passes of its own (the previous encoder ended).
    const auto resume = [&](NSString *label) {
      auto *next = [MTLRenderPassDescriptor renderPassDescriptor];
      next.colorAttachments[0].texture = scene;
      next.colorAttachments[0].loadAction = MTLLoadActionLoad;
      next.colorAttachments[0].storeAction = MTLStoreActionStore;
      next.depthAttachment.texture = raster.depth;
      next.depthAttachment.loadAction = MTLLoadActionLoad;
      next.depthAttachment.storeAction = MTLStoreActionStore;
      encoder = metal::renderEncoder(command, next, label, g.width, g.height);
    };
    // Halo outlines (remixapi_InstanceInfoHaloEXT) composite over the world
    // layers, under the view model and the UI: the frame's layers are split
    // around them. Masked overlays (remixapi_InstanceInfoMaskEXT) likewise
    // split the world layer: the coverage passes follow its unmasked draws.
    const uint32_t halos = raster.haloCount();
    const bool masks = raster.maskRuns() != 0;
    // Frame generation takes the frame as it is before the view model and HUD
    // layers: they have no motion vectors of their own, so the interpolator
    // would warp them by the motion of the scene behind them. Both are drawn
    // over the generated and the rendered frame afterwards.
    const bool hudless = interpolating;
    const bool split = halos || masks || hudless;
    const int sceneLayers = split ? 1 : 3;
    if (tracing) {
      m_pathTracer->record((__bridge void *)encoder);
      // Soft sprite overlays (remixapi_InstanceInfoSpriteEXT) sample the
      // scene's depth, which the composite just wrote: the pass is left for a
      // copy of it.
      if (raster.wantsSceneDepth()) {
        [encoder endEncoding];
        raster.captureSceneDepth(command);
        resume(@"Raster overlay (HUD, effects)");
      }
    }
    raster.record(encoder, metal::Target::Display, tracing, nullptr, g.width, g.height, 0, sceneLayers,
                  masks ? 1 : 0);
    if (masks) {
      [encoder endEncoding];
      raster.recordMasks(command, false);
      resume(@"Raster overlay (masked effects)");
      raster.record(encoder, metal::Target::Display, tracing, nullptr, g.width, g.height, 0, 1, 2);
    }
    for (uint32_t group = 0; group < halos; ++group) {
      [encoder endEncoding];
      raster.recordHaloMask(command, group);
      resume(@"Halo composite");
      raster.recordHaloComposite(encoder, group);
    }
    if (split && !hudless)
      raster.record(encoder, metal::Target::Display, true, nullptr, g.width, g.height, 2, 3);
    if (!interpolating)
      renderUi((__bridge void *)encoder);
    [encoder endEncoding];
    // The rendered frame (UI included) and, with frame generation, the
    // generated one shown before it.
    id<MTLTexture> rendered = interpolating ? frameSlot.real : drawable.texture, generated = nil;
    if (interpolating) {
      if (interpolate(g, *m_pathTracer, command))
        generated = frameSlot.output;
      g.interpolation.previous = g.frameIndex;
      const auto overlay = [&](id<MTLTexture> source, id<MTLTexture> target) {
        if (source) {
          id<MTLBlitCommandEncoder> blit = metal::blitEncoder(command, @"Frame generation: copy color");
          [blit copyFromTexture:source toTexture:target];
          [blit endEncoding];
        }
        auto *ui = [MTLRenderPassDescriptor renderPassDescriptor];
        ui.colorAttachments[0].texture = target;
        ui.colorAttachments[0].loadAction = MTLLoadActionLoad;
        ui.colorAttachments[0].storeAction = MTLStoreActionStore;
        ui.depthAttachment.texture = m_raster.gpu().depth;
        ui.depthAttachment.loadAction = MTLLoadActionClear;
        ui.depthAttachment.storeAction = MTLStoreActionDontCare;
        ui.depthAttachment.clearDepth = 1;
        id<MTLRenderCommandEncoder> uiEncoder =
            metal::renderEncoder(command, ui, @"Raster overlay (view model, HUD) and UI", g.width, g.height);
        raster.record(uiEncoder, metal::Target::Display, true, nullptr, g.width, g.height, 2, 3);
        renderUi((__bridge void *)uiEncoder);
        [uiEncoder endEncoding];
      };
      if (generated) {
        overlay(nil, generated);
        ++g.interpolation.generated;
      }
      overlay(frameSlot.color, rendered);
    }
    if (!m_screenshotPath.empty()) {
      auto &s = g.screenshot;
      s.path = std::move(m_screenshotPath);
      m_screenshotPath.clear();
      s.width = uint32_t(rendered.width);
      s.height = uint32_t(rendered.height);
      s.bgra = true;
      s.readback = metal::buffer(g.device, nullptr, size_t(s.width) * s.height * 4);
      id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
      [blit copyFromTexture:rendered
                       sourceSlice:0
                       sourceLevel:0
                      sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake(s.width, s.height, 1)
                          toBuffer:s.readback
                 destinationOffset:0
            destinationBytesPerRow:s.width * 4
          destinationBytesPerImage:size_t(s.width) * s.height * 4];
      [blit endEncoding];
      s.command = command;
    }
    p.recordMilliseconds = since(recordStart);
    if (drawable)
      [command presentDrawable:drawable];
    dispatch_semaphore_t frames = g.frames;
    std::atomic<bool> *failed = &g.commandFailed;
    if (!interpolating)
      [command addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        if (buffer.status == MTLCommandBufferStatusError) {
          NSLog(@"Remix Metal command buffer failed: %@", buffer.error);
          failed->store(true);
        }
        dispatch_semaphore_signal(frames);
      }];
    else {
      // RTGL-D2-Metal's PresentAsync: once the frame is rendered, the present
      // queue acquires drawables (waiting for the display there, not here),
      // shows the generated frame, then the rendered one half the frame's GPU
      // time later. The pace is kept on the present thread: Metal's paced
      // presentation (presentDrawable:afterMinimumDuration:) held drawables
      // back far longer than asked and starved the frames waiting for them.
      // The slot is free once both are copied.
      CAMetalLayer *layer = g.layer;
      id<MTLCommandQueue> presentQueue = g.interpolation.presentQueue;
      dispatch_queue_t presentDispatch = g.interpolation.presentDispatch;
      [command addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        if (buffer.status == MTLCommandBufferStatusError) {
          NSLog(@"Remix Metal command buffer failed: %@", buffer.error);
          failed->store(true);
        }
        // Half the frame's GPU time: a pace from the measured frame interval
        // would grow with the time frames wait for it.
        const auto pace = std::chrono::duration<double>(std::clamp((buffer.GPUEndTime - buffer.GPUStartTime) * .5, 0., .05));
        dispatch_async(presentDispatch, ^{
          @autoreleasepool {
            // A presenting command buffer for source (uncommitted), or nil.
            const auto show = [&](id<MTLTexture> source) -> id<MTLCommandBuffer> {
              id<CAMetalDrawable> target = [layer nextDrawable];
              // A resize since the frame was rendered: it is not shown.
              if (!target || target.texture.width != source.width || target.texture.height != source.height)
                return nil;
              id<MTLCommandBuffer> present = [presentQueue commandBuffer];
              present.label = @"OpenRemix present";
              id<MTLBlitCommandEncoder> blit = [present blitCommandEncoder];
              [blit copyFromTexture:source toTexture:target.texture];
              [blit endEncoding];
              [present presentDrawable:target];
              return present;
            };
            if (id<MTLCommandBuffer> first = generated ? show(generated) : nil) {
              [first commit];
              std::this_thread::sleep_until(std::chrono::steady_clock::now() + pace);
            }
            // The last on the queue: the slot's textures are copied once it completes.
            id<MTLCommandBuffer> last = show(rendered);
            if (!last)
              last = [presentQueue commandBuffer];
            [last addCompletedHandler:^(id<MTLCommandBuffer>) {
              dispatch_semaphore_signal(frames);
            }];
            [last commit];
          }
        });
      }];
    }
    const auto submitStart = Clock::now();
    [command commit];
    slot.committed = true;
    p.submitMilliseconds = since(submitStart);
    const auto now = Clock::now();
    if (m_lastPresentation != Clock::time_point{})
      m_longestPresentationGap = std::max(m_longestPresentationGap,
                                          std::chrono::duration<double, std::milli>(now - m_lastPresentation).count());
    m_lastPresentation = now;
    ++m_presentationCount;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  // ui/debug_ui.cpp updateFreeCamera.
  void RemixNativeRuntime::updateFreeCamera() {
    if (!m_gui)
      return;
    ui::UiScope scope(m_gui);
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - m_freeCamera.lastTime).count();
    m_freeCamera.lastTime = now;
    if (dt <= 0.0f || dt > 0.1f)
      dt = 1.0f / 60.0f;
    if (!m_freeCamera.enabled || m_freeCamera.locked)
      return;
    const auto &io = ImGui::GetIO();
    if (io.WantTextInput)
      return;
    float speed = m_freeCamera.speed * dt;
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift))
      speed *= 3.0f;
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl))
      speed *= 0.25f;
    float moveForward = 0.0f, moveRight = 0.0f, moveUp = 0.0f;
    if (ImGui::IsKeyDown(ImGuiKey_W)) moveForward += speed;
    if (ImGui::IsKeyDown(ImGuiKey_S)) moveForward -= speed;
    if (ImGui::IsKeyDown(ImGuiKey_D)) moveRight += speed;
    if (ImGui::IsKeyDown(ImGuiKey_A)) moveRight -= speed;
    if (ImGui::IsKeyDown(ImGuiKey_E)) moveUp += speed;
    if (ImGui::IsKeyDown(ImGuiKey_Q)) moveUp -= speed;
    const float turnSpeed = m_freeCamera.turningSpeed * dt * 2.0f;
    const float pitchDir = m_freeCamera.invertY ? -1.0f : 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_J)) m_freeCamera.yaw -= turnSpeed;
    if (ImGui::IsKeyDown(ImGuiKey_L)) m_freeCamera.yaw += turnSpeed;
    if (ImGui::IsKeyDown(ImGuiKey_I)) m_freeCamera.pitch += pitchDir * turnSpeed;
    if (ImGui::IsKeyDown(ImGuiKey_K)) m_freeCamera.pitch -= pitchDir * turnSpeed;
    if (!io.WantCaptureMouse) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float mouseSens = m_freeCamera.turningSpeed * 0.005f;
        m_freeCamera.yaw += io.MouseDelta.x * mouseSens;
        m_freeCamera.pitch -= io.MouseDelta.y * mouseSens * pitchDir;
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        m_freeCamera.reset();
    }
    constexpr float maxPitch = 1.55f;
    m_freeCamera.pitch = std::clamp(m_freeCamera.pitch, -maxPitch, maxPitch);
    constexpr float twoPi = 6.28318530718f;
    while (m_freeCamera.yaw > 3.14159265359f) m_freeCamera.yaw -= twoPi;
    while (m_freeCamera.yaw < -3.14159265359f) m_freeCamera.yaw += twoPi;
    if ((moveForward != 0.0f || moveRight != 0.0f || moveUp != 0.0f) && m_raster.cameraValid()) {
      const scene::Matrix viewToWorld = scene::inverse(m_raster.view());
      const float upSign = viewToWorld[5] < 0.0f ? -1.0f : 1.0f;
      const scene::Matrix rot = scene::getEulerRotationMatrix(upSign * m_freeCamera.pitch, m_freeCamera.yaw);
      scene::Matrix viewToWorldRot = viewToWorld;
      viewToWorldRot[12] = viewToWorldRot[13] = viewToWorldRot[14] = 0.0f;
      viewToWorldRot[15] = 1.0f;
      const scene::Matrix freeCamRot = scene::multiply(viewToWorldRot, rot);
      for (int i = 0; i < 3; ++i)
        m_freeCamera.positionOffset[i] +=
            moveRight * freeCamRot[i] + (upSign * moveUp) * freeCamRot[4 + i] + moveForward * freeCamRot[8 + i];
    }
  }
} // namespace remix
