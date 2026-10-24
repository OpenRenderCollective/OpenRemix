#pragma once
#include "runtime/settings.h"
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
  };

  // The sun NUMOS puts in the scene: its propagation direction and radiance in
  // the renderer's units.
  struct Sun {
    bool active = false;
    float direction[3] = {0, 0, -1};
    float radiance[3] = {};
    float angularDiameterDegrees = .53f;
  };

  // The NUMOS sky in the path tracer: NUMOS's pre-scene work, and the sky for
  // rays that miss the scene baked into an equirectangular environment.
  class Renderer {
  public:
    Renderer(VkPhysicalDevice physical, VkDevice device);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // After the slot's fence.
    void selectFrame(uint32_t slot);
    void setSettings(const config::SkySettings &settings);
    // Records the frame's sky work; false (and nothing recorded) while its
    // pipelines compile or the frame cannot have a sky (status() says why).
    bool record(VkCommandBuffer command, const FrameDescription &frame);
    // After the scene's depth: NUMOS's screen-space clouds. `depthView` is the
    // linear view depth (R32 float, GENERAL, render resolution, 1e10 and
    // above where nothing was hit), or null for an all-sky frame.
    void recordPostDepth(VkCommandBuffer command, VkImage depthImage, VkImageView depthView);

    // The environment as a dome: sampled in GENERAL layout, its texels scaled
    // by environmentScale(), looked up through domeTransform() (world to the
    // map's Y-up frame, rows).
    VkDescriptorImageInfo environment() const;
    float environmentScale() const { return m_environmentScale; }
    const float (&domeTransform() const)[3][3] { return m_domeTransform; }
    const Sun &sun() const { return m_sun; }
    const std::string &status() const { return m_status; }

  private:
    struct State;
    std::unique_ptr<State> m;
    float m_environmentScale = 1;
    float m_domeTransform[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Sun m_sun;
    std::string m_status;
  };
} // namespace remix::sky
