// Particle emitters in the frame (remix_particles_ext.h): the registry's
// frame published at Present, simulated before the target pass and drawn over
// the world.
#include "runtime/runtime.h"
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
    registry.setRendered(m_particles != nullptr);
    if (m_particles) {
      // The last completed frame's counts, and its passes' time when timed.
      auto report = m_particles->report();
      const auto &timings = m_gpuTimers.latest();
      if (timings.available)
        for (const auto &pass : timings.passes)
          if (pass.depth == 0 && std::strncmp(pass.name, "Particles", 9) == 0)
            report.milliseconds += float(pass.milliseconds);
      registry.report(report);
    }
    scene::ParticleRegistry::PublishSettings publish;
    publish.enabled = p.enabled && m_particles != nullptr;
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

  bool RemixNativeRuntime::simulateParticles(VkCommandBuffer buffer) {
    if (!m_particles)
      return false;
    auto &raster = renderer();
    particles::FrameInputs inputs;
    if (!m_particleFrame.enabled) {
      // A disabled frame forgets the particles.
      m_particles->simulate(buffer, m_particleFrame, inputs, {}, m_renderPass);
      return false;
    }
    if (m_loading || !raster.cameraValid() || m_particleFrame.emitters.empty()) {
      m_particles->defer(m_particleFrame);
      return false;
    }
    inputs.viewProjection = raster.camera(REMIXAPI_CAMERA_TYPE_WORLD);
    inputs.view = raster.view(REMIXAPI_CAMERA_TYPE_WORLD);
    inputs.time = float(std::chrono::duration<double>(std::chrono::steady_clock::now() - m_particleStart).count());
    inputs.metersPerUnit = m_traceOptions.sceneScale;
    inputs.upAxis = m_traceOptions.volumetricUpAxis < 3 ? m_traceOptions.volumetricUpAxis : m_particleUpAxis;
    const bool hardwareSrgb =
        m_swapchainFormat == VK_FORMAT_R8G8B8A8_SRGB || m_swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;
    const uint32_t encode = m_srgb && !hardwareSrgb;
    if (m_pathTracer && m_backend != 0) {
      const auto lighting = m_pathTracer->overlayLighting();
      for (int i = 0; i < 3; ++i) {
        inputs.sunDirection[i] = -lighting.sunDirection[i];
        inputs.sunIrradiance[i] = lighting.sunRadiance[i];
        inputs.ambient[i] = lighting.skyRadiance[3] > 0 ? 0.f : lighting.skyRadiance[i];
        for (int c = 0; c < 3; ++c)
          inputs.domeTransform[i][c] = lighting.domeTransform[i][c];
      }
      if (lighting.skyRadiance[3] > 0) {
        inputs.environment = lighting.environment;
        inputs.environmentScale = (lighting.skyRadiance[0] + lighting.skyRadiance[1] + lighting.skyRadiance[2]) / 3;
      }
      inputs.scene = lighting.scene;
      inputs.cloudShadow = lighting.cloudShadow;
      std::memcpy(inputs.cloudShadowTransform, lighting.cloudShadowTransform, sizeof(inputs.cloudShadowTransform));
      inputs.cloudShadowSlices = lighting.cloudShadowSlices;
      inputs.upAxis = m_particleUpAxis = lighting.upAxis;
      // The composite's exposure (postfx/shaders/composite.glsl), as the
      // last completed frame measured it.
      const auto &options = m_traceOptions;
      inputs.exposure = options.exposure * (options.autoExposure ? postProcessingStatistics().exposure : 1.f);
      const uint32_t display[4] = {options.prism, options.gamma22, encode, hardwareSrgb};
      std::copy_n(display, 4, inputs.display);
    } else {
      // The raster backend: the scene's sun and sky colour, unshadowed and
      // shown without a tonemapper.
      const auto &light = raster.light();
      for (int i = 0; i < 3; ++i) {
        inputs.sunDirection[i] = -light.direction[i];
        inputs.sunIrradiance[i] = light.radiance[i];
        inputs.ambient[i] = raster.skyRadiance()[i];
      }
      inputs.exposure = m_profiles.current().sky.rasterExposure;
      inputs.display[2] = encode;
      inputs.display[3] = hardwareSrgb;
    }
    const auto texture = [&raster](uint64_t handle, VkCommandBuffer command) -> VkDescriptorImageInfo {
      const auto it = raster.textures().find(handle);
      if (it == raster.textures().end() || !it->second)
        return {};
      return raster.sampledTexture(raster.defaultMaterial(), it->second, command);
    };
    if (!m_particles->simulate(buffer, m_particleFrame, inputs, texture, m_renderPass))
      return false;
    // Soft edges sample the scene's depth.
    raster.requestSceneDepth();
    return true;
  }

  void RemixNativeRuntime::drawParticles(VkCommandBuffer buffer, VkRenderPassBeginInfo &pass) {
    auto &raster = renderer();
    if (!raster.sceneDepth().imageView && raster.wantsSceneDepth()) {
      // The scene's depth stays in the target's depth attachment.
      gpu::GpuScope depthTime(buffer, "Scene depth for soft overlays", m_swapchainExtent);
      vkCmdEndRenderPass(buffer);
      raster.captureSceneDepth(buffer);
      pass.renderPass = m_resumePass;
      vkCmdBeginRenderPass(buffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    }
    m_particles->draw(buffer, m_swapchainExtent, raster.sceneDepth());
  }
} // namespace remix
