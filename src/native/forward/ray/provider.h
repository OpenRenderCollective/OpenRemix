#pragma once
#include "forward/ray/reflections.h"
#include "forward/provider_plan.h"

namespace remix::forward::ray {
  // Owns the live scene shared by ray-query diffuse, reflections and water.
  // Technique selection is resolved at initialization, outside draw loops.
  // Frame selection requires the caller to have waited for the slot fence.
  class Provider {
  public:
    static techniques::Capabilities capabilities(VkPhysicalDevice physical, bool hardwareRay);
    void initialize(VkPhysicalDevice physical, VkDevice device, const techniques::ResourcePlan &plan,
                     techniques::Capabilities capabilities);
    void reset();
    void selectFrame(uint32_t slot) {
      if (m_scene) m_scene->selectFrame(slot);
      if (m_reflections) m_reflections->selectFrame(slot);
    }
    struct FrameInputs {
      VkDescriptorSet diffuse = VK_NULL_HANDLE;
      VkDescriptorSet water = VK_NULL_HANDLE;
      float reflectionDistance = 0; // scene units, from the active transport snapshot
      uint32_t reflectionBounces = 0;
    };
    FrameInputs prepare(VkCommandBuffer command, RasterRenderer &resources,
                        const Options &options, double animationSeconds, uint32_t slot);
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
