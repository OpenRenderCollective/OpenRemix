// Particle emitters in the frame (remix_particles_ext.h; runtime/particles.cpp):
// the registry's frame published at Present, simulated before the target pass
// and drawn over the world.
#include "metal/particles.h"
#include "metal/runtime_gpu.h"
#include "scene/particles.h"
#include <algorithm>
#include <cstring>

namespace remix {
  void RemixNativeRuntime::publishParticles() {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = m_particleClock == std::chrono::steady_clock::time_point{}
                               ? 0.0
                               : std::chrono::duration<double>(now - m_particleClock).count();
    m_particleClock = now;
    const auto &settings = m_profiles.current();
    const auto &p = settings.particles;
    auto &registry = scene::particles();
    auto *particles = m_gpu ? m_gpu->particles.get() : nullptr;
    registry.setRendered(particles != nullptr);
    if (particles) {
      // The last completed frame's counts, and its passes' time when timed.
      auto report = particles->report();
      if (m_gpuTimings.available)
        for (const auto &pass : m_gpuTimings.passes)
          if (pass.depth == 0 && std::strncmp(pass.name, "Particles", 9) == 0)
            report.milliseconds += float(pass.milliseconds);
      registry.report(report);
    }
    scene::ParticleRegistry::PublishSettings publish;
    publish.enabled = p.enabled && particles != nullptr;
    publish.seconds = seconds;
    publish.budget = p.budget;
    publish.spawnBudget = p.spawnBudget;
    publish.metersPerUnit = settings.trace.sceneScale;
    // Auto (3) is resolved by the path tracer from the camera.
    publish.upAxis = settings.trace.volumetricUpAxis < 3 ? settings.trace.volumetricUpAxis : m_particleUpAxis;
    // The sky's weather when it runs (the game's or its preset's), else the setting.
    const auto sky = skyState();
    publish.precipitation = sky.active ? sky.precipitation : p.precipitation;
    publish.rain = p.rain;
    publish.snow = p.snow;
    publish.shelter = p.shelter;
    publish.followWeather = p.followWeather;
    publish.intensity = p.intensity;
    m_particleFrame = registry.publish(publish);
  }

  bool RemixNativeRuntime::simulateParticles(void *commandBuffer) {
    auto &g = *m_gpu;
    if (!g.particles)
      return false;
    id<MTLCommandBuffer> command = (__bridge id<MTLCommandBuffer>)commandBuffer;
    auto &raster = renderer().gpu();
    metal::Particles::Inputs inputs;
    if (!m_particleFrame.enabled) {
      // A disabled frame forgets the particles.
      g.particles->simulate(command, m_particleFrame, inputs, {}, raster.displayFormat, raster.depthFormat);
      return false;
    }
    // While the path tracer prepares (m_loading) the raster frame is drawn,
    // particles included.
    if (!renderer().cameraValid() || m_particleFrame.emitters.empty()) {
      g.particles->defer(m_particleFrame);
      return false;
    }
    inputs.viewProjection = renderer().camera(REMIXAPI_CAMERA_TYPE_WORLD);
    inputs.view = renderer().view(REMIXAPI_CAMERA_TYPE_WORLD);
    inputs.time = float(std::chrono::duration<double>(std::chrono::steady_clock::now() - m_particleStart).count());
    inputs.metersPerUnit = m_traceOptions.sceneScale;
    inputs.upAxis = m_traceOptions.volumetricUpAxis < 3 ? m_traceOptions.volumetricUpAxis : m_particleUpAxis;
    // Shown as the composite shows the scene (displayColor).
    const auto display = displayOutput();
    inputs.display[0] = display.hdr10 ? 1u : 0u;
    std::memcpy(&inputs.display[1], &display.peakNits, sizeof(float));
    std::memcpy(&inputs.display[2], &display.paperWhiteNits, sizeof(float));
    if (m_pathTracer && m_backend != 0) {
      const auto lighting = m_pathTracer->overlayLighting();
      for (int i = 0; i < 3; ++i) {
        inputs.sunDirection[i] = -lighting.sunDirection[i];
        inputs.sunIrradiance[i] = lighting.sunRadiance[i];
        inputs.ambient[i] = lighting.skyRadiance[3] > 0 ? 0.f : lighting.skyRadiance[i];
        for (int c = 0; c < 3; ++c)
          inputs.domeTransform[i][c] = lighting.domeTransform[i][c];
      }
      if (lighting.skyRadiance[3] > 0 && lighting.environment) {
        inputs.environment = (__bridge id<MTLTexture>)lighting.environment;
        inputs.environmentSampler = (__bridge id<MTLSamplerState>)lighting.environmentSampler;
        inputs.environmentScale = (lighting.skyRadiance[0] + lighting.skyRadiance[1] + lighting.skyRadiance[2]) / 3;
      }
      inputs.scene = (__bridge id<MTLAccelerationStructure>)lighting.scene;
      for (void *resource : lighting.referenced)
        inputs.referenced.push_back((__bridge id<MTLResource>)resource);
      inputs.cloudShadow = (__bridge id<MTLTexture>)lighting.cloudShadow;
      inputs.cloudShadowSampler = (__bridge id<MTLSamplerState>)lighting.cloudShadowSampler;
      std::memcpy(inputs.cloudShadowTransform, lighting.cloudShadowTransform, sizeof(inputs.cloudShadowTransform));
      inputs.cloudShadowSlices = lighting.cloudShadowSlices;
      inputs.upAxis = m_particleUpAxis = lighting.upAxis;
      // The composite's exposure (postfx/shaders/composite.glsl), as the
      // last completed frame measured it.
      const auto &options = m_traceOptions;
      inputs.exposure = options.exposure * (options.autoExposure ? m_pathTracer->postStatistics().exposure : 1.f);
      inputs.display[0] |= lighting.skyRadiance[3] > 0 && lighting.environmentSrgb ? 2u : 0u;
    } else {
      // The raster backend: the scene's sun and sky colour, unshadowed and
      // shown without a tonemapper.
      const auto &light = renderer().light();
      for (int i = 0; i < 3; ++i) {
        inputs.sunDirection[i] = -light.direction[i];
        inputs.sunIrradiance[i] = light.radiance[i];
        inputs.ambient[i] = renderer().skyRadiance()[i];
      }
      inputs.exposure = frameSettings().sky.rasterExposure;
    }
    const auto texture = [this](uint64_t handle) -> id<MTLTexture> {
      const auto it = renderer().textures().find(handle);
      if (it == renderer().textures().end() || !it->second)
        return nil;
      return renderer().gpu().sampledTexture(renderer().defaultMaterial(), it->second).texture;
    };
    if (!g.particles->simulate(command, m_particleFrame, inputs, texture, raster.displayFormat, raster.depthFormat))
      return false;
    // Soft edges sample the scene's depth.
    raster.sprite.requested = true;
    return true;
  }

  void RemixNativeRuntime::drawParticles(void *renderEncoder) {
    auto &g = *m_gpu;
    auto &raster = renderer().gpu();
    id<MTLRenderCommandEncoder> encoder = (__bridge id<MTLRenderCommandEncoder>)renderEncoder;
    g.particles->draw(encoder, g.width, g.height, raster.sprite.captured ? raster.sprite.depth : nil);
  }
} // namespace remix
