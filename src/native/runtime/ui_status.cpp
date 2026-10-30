// The developer UI's view of the Vulkan runtime (ui/status.h): what the shared
// tabs (ui/rendering.cpp, lighting.cpp, post_processing.cpp, diagnostics.cpp,
// textures.cpp) ask of the backend, answered from its own state. metal/ui.mm
// answers the same queries in Metal builds.
#include "runtime/runtime.h"
#include "upscale/ngx.h"
#include <chrono>
#include <cstdio>
#include <string_view>

namespace remix {
  bool RemixNativeRuntime::reconstructionDenoises() const {
    return m_pathTracer && m_pathTracer->upscalingStatistics().backend == upscale::Backend::DlssRr;
  }

  ui::ReconstructionStatus RemixNativeRuntime::uiReconstruction() const {
    const auto s = m_pathTracer ? m_pathTracer->upscalingStatistics() : upscale::Statistics{};
    ui::ReconstructionStatus result;
    result.name = (s.backend == upscale::Backend::Fsr31 || s.backend == upscale::Backend::Xess) && !s.provider.empty()
                      ? s.provider
                      : upscale::name(s.backend);
    result.active = s.active;
    result.denoises = result.models = s.backend == upscale::Backend::DlssRr;
    result.cnn = s.rrCnn;
    result.preset = s.rrPreset;
    result.note = s.rrNote;
    result.renderWidth = s.render.width;
    result.renderHeight = s.render.height;
    result.displayWidth = s.display.width;
    result.displayHeight = s.display.height;
    result.milliseconds = s.milliseconds;
    result.failure = s.failure;
    return result;
  }

  ui::ReconstructionChoices RemixNativeRuntime::uiReconstructionChoices() const {
    ui::ReconstructionChoices result;
    // XeSS: Windows only (upscale/xess.h).
#if REMIX_HAS_XESS
    result.upscalers = "Off\0Auto\0FSR\0DLSS\0XeSS\0";
#endif
    return result;
  }

  ui::FrameGenerationStatus RemixNativeRuntime::uiFrameGeneration() const {
    ui::FrameGenerationStatus result;
    result.status = m_frameGenerationStatus;
    if (m_ngx) {
      const auto ngx = m_ngx->capabilities();
      if (ngx.initializationComplete && !ngx.fg)
        result.unavailable = "DLSS Frame Generation unavailable: " + ngx.fgReason;
    }
    result.generated = m_generatedFrames;
    result.duplicated = m_duplicatedFrames;
    result.ungenerated = m_ungeneratedFrames;
    result.renderedInterval = m_renderedInterval;
    result.presentImages = m_swapchainImages.size();
    result.presentMode = m_presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR  ? "immediate"
                         : m_presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                         : m_presentMode == VK_PRESENT_MODE_FIFO_KHR    ? "FIFO (vsync)"
                                                                        : "other present mode";
    result.lowLatencySupported = m_lowLatencySupported;
    result.lowLatencyUnavailable = "Needs an NVIDIA GPU and driver with VK_NV_low_latency2, and the path tracer.";
    return result;
  }

  ui::SkyStatus RemixNativeRuntime::uiSky() const {
    ui::SkyStatus status;
    if (!m_pathTracer) {
      // The raster backend's sky (raster_sky.cpp).
      status.supported = m_skyUnavailable.empty();
      status.active = m_rasterSkyActive;
      status.reason = !status.supported             ? "The device lacks " + m_skyUnavailable + "."
                      : !m_rasterSkyFailure.empty() ? "Stopped: " + m_rasterSkyFailure
                                                    : std::string();
    } else {
      const auto sky = m_pathTracer->skyStatus();
      status.supported = sky.supported;
      status.active = sky.active;
      status.reason = sky.reason;
    }
    // With GPU pass timers on: the sky's passes ("NUMOS sky", "NUMOS screen
    // clouds", "NUMOS composite").
    for (const auto &pass : m_gpuTimers.latest().passes)
      if (std::string_view(pass.name).starts_with("NUMOS "))
        status.gpuMilliseconds += float(pass.milliseconds);
    return status;
  }

  ui::PostStatus RemixNativeRuntime::uiPost() const {
    const auto post = postProcessingStatistics();
    ui::PostStatus result;
    result.exposure = post.exposure;
    result.target = post.target;
    result.limit = post.limit < 0 ? -1 : post.limit > 0 ? 1 : 0;
    result.milliseconds = post.milliseconds;
    result.memoryBytes = post.memoryBytes;
    result.activeExposure = m_postOptions.exposure * (m_postOptions.autoExposure ? post.exposure : 1.f);
    result.failure = m_postFailure;
    return result;
  }

  ui::HdrStatus RemixNativeRuntime::uiHdr() const {
    ui::HdrStatus result;
    result.available = !m_headless && m_window.hdrEnabled();
    result.active = m_hdrOutput.active();
    result.inactive = m_hdrStatus;
    return result;
  }

  ui::ScreenshotStatus RemixNativeRuntime::uiScreenshots() const { return {!m_headless, m_screenshot.status()}; }

  // Taken by menuShortcut: a request may wait for the render thread, which draws the panel.
  void RemixNativeRuntime::uiTakeScreenshot() { m_screenshotButton = true; }

  ui::DeviceStatus RemixNativeRuntime::uiDevice() const {
    char api[32];
    std::snprintf(api, sizeof(api), "Vulkan %u.%u.%u", VK_VERSION_MAJOR(m_properties.apiVersion),
                  VK_VERSION_MINOR(m_properties.apiVersion), VK_VERSION_PATCH(m_properties.apiVersion));
    return {m_properties.deviceName, api};
  }

  ui::PreparationStatus RemixNativeRuntime::uiPreparation() const {
    ui::PreparationStatus result;
    if (!m_pipelines)
      return result;
    const auto s = m_pipelines->statistics();
    result.available = true;
    result.completed = s.completed;
    result.requested = s.requested;
    result.created = s.created;
    result.reused = s.reused;
    result.failed = s.failed;
    result.deduplicated = s.deduplicated;
    result.cacheHits = s.cacheHits;
    result.milliseconds = s.milliseconds;
    result.driverMilliseconds = double(s.driverNanoseconds) / 1e6;
    result.cacheLoaded = s.cacheLoaded;
    result.feedback = s.feedback;
    result.cacheWriteFailed = s.cacheWriteFailed;
    if (m_preparation) {
      result.pending = true;
      result.generation = m_preparation->generation;
      result.elapsedSeconds =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - m_preparation->started).count();
    }
    result.failure = m_preparationFailure;
    return result;
  }

  ui::PresentStatus RemixNativeRuntime::uiPresent() const {
    const auto side = [](const AsyncPresentStatistics &s) {
      ui::AsyncPresent result;
      result.waitMilliseconds = s.waitMilliseconds;
      result.setupMilliseconds = s.setupMilliseconds;
      result.captureMilliseconds = s.captureMilliseconds;
      result.uiMilliseconds = s.uiMilliseconds;
      result.callMilliseconds = s.callMilliseconds;
      result.renderMilliseconds = s.renderMilliseconds;
      result.handedOff = s.handedOff;
      result.rendered = s.rendered;
      return result;
    };
    ui::PresentStatus result;
    result.performance = m_previousPerformance;
    result.breakdown = m_previousBreakdown;
    result.async = m_asyncActive;
    result.caller = side(m_previousAsyncCaller);
    result.current = side(m_asyncCaller);
    result.render = side(m_asyncRender);
    const auto &st = m_previousStatic;
    auto &statics = result.statics;
    statics.active = st.active;
    statics.units = st.units;
    statics.surfaces = st.surfaces;
    statics.reusedSurfaces = st.reusedSurfaces;
    statics.rebuiltSurfaces = st.rebuiltSurfaces;
    statics.dynamicSurfaces = st.dynamicSurfaces;
    statics.rebuiltUnits = st.rebuiltUnits;
    statics.changes = st.changes;
    statics.temporalSurfaces = st.temporalSurfaces;
    statics.relayouts = st.relayouts;
    statics.syncMilliseconds = st.syncMilliseconds;
    statics.tableMilliseconds = st.tableMilliseconds;
    statics.validations = st.validations;
    statics.mismatches = st.mismatches;
    statics.mismatch = st.mismatch;
    return result;
  }

  ui::PathInspectionStatus RemixNativeRuntime::uiPathInspection() const {
    ui::PathInspectionStatus result;
    if (!m_pathTracer)
      return result;
    result.available = true;
    const auto extent = m_pathTracer->renderExtent();
    result.renderWidth = extent.width;
    result.renderHeight = extent.height;
    result.inspection = m_pathTracer->pathInspection();
    return result;
  }

  void RemixNativeRuntime::uiInspectPixel(int x, int y) {
    if (m_pathTracer)
      m_pathTracer->inspectPixel(x, y);
  }
} // namespace remix
