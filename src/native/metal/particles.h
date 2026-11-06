#pragma once
// Objective-C++ only: particle emitters on Metal (particles/renderer.h's
// counterpart). The same shaders, translated from the same SPIR-V, and the
// same passes: the light volume, simulation, spawns and indirect draws
// outside the target pass, then one indirect draw per emitter inside it.
#include "metal/gpu.h"
#include "scene/particles.h"
#include "scene/types.h"
#include <functional>
#include <memory>

namespace remix::metal {
  class Particles {
  public:
    // What a frame gives the particles, in world units (particles::FrameInputs).
    struct Inputs {
      scene::Matrix viewProjection = scene::identity(); // the world camera, as raster overlays
      scene::Matrix view = scene::identity();
      float time = 0; // seconds since start, for turbulence
      float metersPerUnit = 1;
      uint32_t upAxis = 2;
      float sunDirection[3] = {0, 0, 1}; // toward the sun
      float sunIrradiance[3] = {};
      float ambient[3] = {}; // the sky's mean radiance without an environment
      // The path tracer's environment and the scene's TLAS (with what its
      // instances reference) for the sun's visibility and SHELTER. nil: none.
      id<MTLTexture> environment = nil;
      id<MTLSamplerState> environmentSampler = nil;
      float environmentScale = 1;
      float domeTransform[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
      id<MTLAccelerationStructure> scene = nil;
      std::vector<id<MTLResource>> referenced;
      // The clouds' shadow on the sun (sky::Renderer::cloudShadow), or nil.
      id<MTLTexture> cloudShadow = nil;
      id<MTLSamplerState> cloudShadowSampler = nil;
      float cloudShadowTransform[3][4] = {};
      uint32_t cloudShadowSlices = 0;
      float exposure = 1;
      // particles::FrameInputs::display.
      uint32_t display[4] = {};
    };

    // rayQuery: the device traces rays (the traced passes).
    Particles(id<MTLDevice> device, bool rayQuery);
    ~Particles();
    Particles(const Particles &) = delete;
    Particles &operator=(const Particles &) = delete;

    // After the slot's command buffer completed: takes the statistics of the frame it held.
    void selectFrame(uint32_t slot);
    // An emitter texture, or nil.
    using TextureLookup = std::function<id<MTLTexture>(uint64_t texture)>;
    // Outside a pass; `target` the pixel formats draw() renders to. False when
    // nothing is to be drawn (no emitters, pipelines compiling).
    bool simulate(id<MTLCommandBuffer> command, const scene::ParticleFrame &frame, const Inputs &inputs,
                  const TextureLookup &texture, MTLPixelFormat color, MTLPixelFormat depth);
    // A frame that is not simulated (no camera): its resets wait for the next.
    void defer(const scene::ParticleFrame &frame);
    // Inside the target pass, after simulate() returned true. sceneDepth: the
    // frame's captured opaque depth, or nil (hard edges).
    void draw(id<MTLRenderCommandEncoder> encoder, uint32_t width, uint32_t height, id<MTLTexture> sceneDepth);
    scene::ParticleRegistry::GpuReport report() const { return m_report; }

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m;
    scene::ParticleRegistry::GpuReport m_report;
  };
} // namespace remix::metal
