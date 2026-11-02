#pragma once
#include "forward/ray/reflections.h"

namespace remix::forward::ray {
  // Owns the live scene shared by ray-query diffuse, reflections and water.
  // Technique selection is resolved at initialization, outside draw loops.
  // Frame selection requires the caller to have waited for the slot fence.
  class Provider {
  public:
    void initialize(VkPhysicalDevice physical, VkDevice device, const Options &options, bool hardwareRay);
    void reset();
    void selectFrame(uint32_t slot) {
      if (m_scene) m_scene->selectFrame(slot);
      if (m_reflections) m_reflections->selectFrame(slot);
    }
    bool active() const { return bool(m_scene); }
    struct FrameInputs {
      VkDescriptorSet diffuse = VK_NULL_HANDLE;
      VkDescriptorSet water = VK_NULL_HANDLE;
    };
    FrameInputs prepare(VkCommandBuffer command, RasterRenderer &resources,
                        const Options &options, double animationSeconds, uint32_t slot);
    bool diffuseActive() const { return m_diffuse; }
    bool waterActive() const { return m_water; }
    const Statistics &diffuseStatistics() const { return m_diffuseStatistics; }
    const Statistics &reflectionStatistics() const {
      return m_reflections ? m_reflections->statistics() : m_reflectionStatistics;
    }
    Reflections *reflections() const { return m_reflections.get(); }
    LiveScene::Memory memory() const {
      auto result = m_scene ? m_scene->memory() : LiveScene::Memory{};
      if (m_reflections) {
        const auto reflections = m_reflections->memory();
        result.buffers += reflections.buffers;
        result.images += reflections.images;
        result.acceleration += reflections.acceleration;
      }
      return result;
    }

  private:
    bool m_diffuse = false, m_water = false;
    Statistics m_diffuseStatistics, m_reflectionStatistics;
    // Destruction order matters: reflections retain references to the scene.
    std::unique_ptr<LiveScene> m_scene;
    std::unique_ptr<Reflections> m_reflections;
  };
}
