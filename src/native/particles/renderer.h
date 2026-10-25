#pragma once
#include "scene/particles.h"
#include "scene/types.h"
#include <functional>
#include <memory>
#include <vulkan/vulkan.h>

// Particle emitters on the GPU (remix_particles_ext.h): a pool of particles
// in two buffers that alternate each frame, each emitter a slice of both.
// simulate() records the frame's compute passes (light volume, simulation,
// spawns, indirect draws) outside a render pass; draw() records one indirect
// draw per emitter inside the target pass, after the world and before the
// view model. Nothing here is traced: the particles cost the path tracer
// only the light volume's shadow rays and SHELTER's spawn rays.
namespace remix::particles {
  // What a frame gives the particles, in world units.
  struct FrameInputs {
    scene::Matrix viewProjection = scene::identity(); // the world camera, as raster overlays
    scene::Matrix view = scene::identity();
    float time = 0;                     // seconds since start, for turbulence
    float metersPerUnit = 1;
    uint32_t upAxis = 2;
    float sunDirection[3] = {0, 0, 1};  // toward the sun
    float sunIrradiance[3] = {};
    float ambient[3] = {};              // the sky's mean radiance without an environment
    // The path tracer's environment (sampled for the sky's mean radiance) and
    // the scene's TLAS (the sun's visibility, SHELTER). Null: none.
    VkDescriptorImageInfo environment{};
    float environmentScale = 1;
    float domeTransform[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    VkAccelerationStructureKHR scene = VK_NULL_HANDLE;
    // The clouds' shadow on the sun (sky::Renderer::cloudShadow), or a null view.
    VkDescriptorImageInfo cloudShadow{};
    float cloudShadowTransform[3][4] = {};
    uint32_t cloudShadowSlices = 0;
    float exposure = 1;
    uint32_t display[4] = {}; // prism, gamma 2.2, encode, hardware sRGB (display.glsl)
  };

  class Renderer {
  public:
    // rayQuery: the device has ray queries enabled (the traced passes).
    Renderer(VkPhysicalDevice physical, VkDevice device, bool rayQuery);
    ~Renderer(); // the device is idle
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // After the slot's fence: takes the statistics of the frame it held.
    void selectFrame(uint32_t slot);
    // An emitter texture's descriptor (outside a render pass; it may record
    // the texture's upload), or a null view.
    using TextureLookup = std::function<VkDescriptorImageInfo(uint64_t texture, VkCommandBuffer command)>;
    // Outside a render pass; `target` is the pass draw() records into. False
    // when nothing is to be drawn (no emitters, pipelines compiling).
    bool simulate(VkCommandBuffer command, const scene::ParticleFrame &frame, const FrameInputs &inputs,
                  const TextureLookup &texture, VkRenderPass target);
    // A frame that is not simulated (no camera): its resets wait for the next.
    void defer(const scene::ParticleFrame &frame);
    // Inside the target pass, after simulate() returned true. sceneDepth: the
    // frame's captured opaque depth (in the layout it names), or a null view
    // (hard edges).
    void draw(VkCommandBuffer command, VkExtent2D extent, VkDescriptorImageInfo sceneDepth);
    scene::ParticleRegistry::GpuReport report() const { return m_report; }

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m;
    scene::ParticleRegistry::GpuReport m_report;
  };
} // namespace remix::particles
