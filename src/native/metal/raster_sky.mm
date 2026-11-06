#include "metal/runtime_gpu.h"
#include <cstdio>
#include <exception>

// The raster backend's NUMOS sky (runtime/raster_sky.cpp). Before the target
// pass NUMOS records its frame for the world camera; after the world's layers
// the pass is left, the target's depth goes to the screen clouds and the
// composite, and the pass is resumed to blend the sky over the frame
// (Raster::Gpu::recordSkyComposite). No scene TLAS: the haze is unshadowed and
// the ground level is configured.
namespace remix {
  bool RemixNativeRuntime::recordRasterSky(void *commandBuffer) {
    auto &s = m_gpu->rasterSky;
    s.active = false;
#if REMIX_HAS_NUMOS
    const auto &settings = frameSettings();
    if (m_pathTracer || !settings.sky.enabled || !m_traceSupported || !s.failure.empty()) {
      s.ready = false;
      return false;
    }
    id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)commandBuffer;
    try {
      if (!s.renderer)
        s.renderer = std::make_shared<sky::Renderer>(nullptr, m_gpu->device, m_traceSupported);
      // This slot's last frame has completed.
      s.renderer->selectFrame(m_gpu->frameIndex);
      s.renderer->setSettings(settings.sky, frameSkyControl());
      const auto &view = renderer().view(), &projection = renderer().projection();
      s.upAxis.observe(scene::inverse(view));
      const sky::FrameDescription description{
          .view = view,
          .projection = projection,
          .previousView = s.ready ? s.view : view,
          .previousProjection = s.ready ? s.projection : projection,
          .renderWidth = m_gpu->width,
          .renderHeight = m_gpu->height,
          .outputWidth = m_gpu->width,
          .outputHeight = m_gpu->height,
          .metersPerUnit = settings.trace.sceneScale,
          .upAxis = s.upAxis.resolve(settings.trace.volumetricUpAxis),
          .cameraCut = !s.ready};
      s.active = s.renderer->record(command, description);
      s.ready |= s.active;
      s.view = view;
      s.projection = projection;
    } catch (const std::exception &exception) {
      s.failure = exception.what();
    }
    if (!s.failure.empty()) {
      std::fprintf(stderr, "NUMOS sky stopped: %s\n", s.failure.c_str());
      s.active = s.ready = false;
    }
#else
    (void)commandBuffer;
#endif
    return s.active;
  }

  bool RemixNativeRuntime::composeRasterSky(void *commandBuffer) {
    auto &s = m_gpu->rasterSky;
#if REMIX_HAS_NUMOS
    auto &raster = renderer().gpu();
    if (!s.active || !raster.depth)
      return false;
    const auto &settings = frameSettings();
    id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)commandBuffer;
    bool composed = false;
    try {
      s.renderer->recordRasterPostDepth(command, raster.depth, raster.depth, renderer().projection(), 1.f);
      composed = s.renderer->recordRasterComposite(command, settings.sky.rasterExposure * settings.sky.radianceScale,
                                                   settings.sky.gameSky);
    } catch (const std::exception &exception) {
      s.failure = exception.what();
    }
    if (!s.failure.empty()) {
      std::fprintf(stderr, "NUMOS sky stopped: %s\n", s.failure.c_str());
      s.active = s.ready = false;
      return false;
    }
    return composed && s.renderer->rasterTransmittance().texture && s.renderer->rasterAddition().texture;
#else
    (void)commandBuffer;
    return false;
#endif
  }
} // namespace remix
