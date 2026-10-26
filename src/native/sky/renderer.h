#pragma once
#include "runtime/settings.h"
#include "runtime/sky_control.h"
#include "scene/types.h"
#include <chrono>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace remix::sky {
  class Bridge;

  // What a frame gives the sky: its cameras and resolution, in world units.
  struct FrameDescription {
    scene::Matrix view{}, projection{}, previousView{}, previousProjection{};
    float jitter[2] = {}; // pixels
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    float metersPerUnit = 1;
    uint32_t upAxis = 2; // 0 x, 1 y, 2 z
    bool cameraCut = false;
    // The scene's TLAS, built this frame before record(), for the haze's
    // shadows and the ground probes: NUMOS traces its opaque world bit
    // (trace::skyOccluderInstanceMask). Null: unshadowed haze.
    VkAccelerationStructureKHR scene = VK_NULL_HANDLE;
  };

  // The distant light NUMOS puts in the scene (its sun, a moon or a lightning
  // flash): its propagation direction and radiance in the renderer's units.
  struct Sun {
    bool active = false;
    // The clouds' shadow (Renderer::cloudShadow) applies to it.
    bool cloudShadowed = false;
    float direction[3] = {0, 0, -1};
    float radiance[3] = {};
    float angularDiameterDegrees = .53f;
  };

  // The NUMOS sky in the path tracer: NUMOS's pre-scene work, and the sky for
  // rays that miss the scene baked into an equirectangular environment.
  class Renderer {
  public:
    // rayQuery: the device traces rays (the scene TLAS may be given).
    Renderer(VkPhysicalDevice physical, VkDevice device, bool rayQuery = true);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // After the slot's fence.
    void selectFrame(uint32_t slot);
    // The profile's sky settings and the game's control over them
    // (remix_sky_ext.h), before record().
    void setSettings(const config::SkySettings &settings, const config::SkyControl &control);
    // What the sky did in the last record() (inactive while it has not run).
    const config::SkyState &state() const;
    // Records the frame's sky work; false (and nothing recorded) while its
    // pipelines compile or the frame cannot have a sky (status() says why).
    bool record(VkCommandBuffer command, const FrameDescription &frame);
    // After the scene's depth: NUMOS's screen-space clouds. `depthView` is the
    // linear view depth (R32 float, GENERAL, render resolution, 1e10 and
    // above where nothing was hit), or null for an all-sky frame.
    void recordPostDepth(VkCommandBuffer command, VkImage depthImage, VkImageView depthView);
    // NUMOS over the composed frame before upscaling, in place: `color` is
    // RGBA16 float in GENERAL at render resolution, `depth` the view depth of
    // recordPostDepth. `scale` turns NUMOS radiance into the frame's.
    void recordComposite(VkCommandBuffer command, VkImage colorImage, VkImageView colorView, VkImage depthImage,
                         VkImageView depthView, float scale, config::SkySettings::GameSky gameSky);

    // The raster renderer's sky, after record() and the world's draws:
    // recordRasterPostDepth takes the target's depth (a depth-aspect view in
    // GENERAL layout, cleared to clearDepth where nothing was drawn) and runs
    // the screen clouds; recordRasterComposite then writes what the target is
    // multiplied by and what is added after (rasterTransmittance,
    // rasterAddition: RGBA16 float, GENERAL, render resolution). exposure
    // turns NUMOS radiance into the display's (gamma 2.2 encoded, as the
    // UNORM target blends).
    void recordRasterPostDepth(VkCommandBuffer command, VkImage depthImage, VkImageView depthView,
                               const scene::Matrix &projection, float clearDepth);
    bool recordRasterComposite(VkCommandBuffer command, float exposure, config::SkySettings::GameSky gameSky);
    VkDescriptorImageInfo rasterAddition() const;
    VkDescriptorImageInfo rasterTransmittance() const;

    // The environment as a dome: sampled in GENERAL layout, its texels scaled
    // by environmentScale(), looked up through domeTransform() (world to the
    // map's Y-up frame, rows).
    VkDescriptorImageInfo environment() const;
    float environmentScale() const { return m_environmentScale; }
    const float (&domeTransform() const)[3][3] { return m_domeTransform; }
    const Sun &sun() const { return m_sun; }
    // The clouds' transmittance toward the sun around the camera: one R16
    // float texture in GENERAL layout holding cloudShadowSlices horizontal
    // slices side by side. cloudShadowTransform() rows take a world position
    // (w = 1) to (u, v, slice) in [0, 1]; null before the first bake.
    VkDescriptorImageInfo cloudShadow() const;
    const float (&cloudShadowTransform() const)[3][4] { return m_cloudShadowTransform; }
    static constexpr uint32_t cloudShadowSlices = 4;
    const std::string &status() const { return m_status; }

  private:
    struct State;
    std::unique_ptr<State> m;
    float m_environmentScale = 1;
    float m_domeTransform[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float m_cloudShadowTransform[3][4] = {};
    Sun m_sun;
    std::string m_status;
  };
} // namespace remix::sky
