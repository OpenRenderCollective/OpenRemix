#pragma once
#include "forward/cascades/cascades.h"
#include "forward/ray/reflections.h"
#include "forward/provider_plan.h"

namespace remix::forward::ray {
  // Owns the live scene shared by ray-query diffuse, radiance cascades,
  // reflections and water. Technique selection is resolved at initialization,
  // outside draw loops. Frame selection requires the caller to have waited for the slot fence.
  class Provider {
  public:
    static techniques::Capabilities capabilities(VkPhysicalDevice physical, bool hardwareRay);
    void initialize(VkPhysicalDevice physical, VkDevice device, const techniques::ResourcePlan &plan,
                     techniques::Capabilities capabilities);
    // Target-sized cascade resources, once the world targets exist. No-op
    // unless the plan selected the radiance cascade diffuse provider.
    void initializeCascades(VkExtent2D extent, VkSampleCountFlagBits samples, const Options &options);
    void reset();
    ~Provider() { reset(); }
    void selectFrame(uint32_t slot) {
      if (m_scene) m_scene->selectFrame(slot);
      if (m_reflections) m_reflections->selectFrame(slot);
      if (m_cascades) m_cascades->selectFrame(slot);
    }
    struct FrameInputs {
      VkDescriptorSet diffuse = VK_NULL_HANDLE;
      VkDescriptorSet water = VK_NULL_HANDLE;
      VkDescriptorSet cascades = VK_NULL_HANDLE; // the cascade trace's scene set
      float reflectionDistance = 0; // scene units, from the active transport snapshot
      uint32_t reflectionBounces = 0;
    };
    // sun: the frame's live distant light (the scene's, or a panel override).
    FrameInputs prepare(VkCommandBuffer command, RasterRenderer &resources, const Options &options,
                        const scene::DistantLight &sun, double animationSeconds, uint32_t slot);
    // After the world pass: this frame's cascade gather, read by the next frame's shading.
    void recordCascades(VkCommandBuffer command, const cascades::RadianceCascades::Inputs &inputs,
                        const Options &options);
    const cascades::RadianceCascades *cascades() const { return m_cascades.get(); }
    const cascades::Statistics &cascadeStatistics() const { return m_cascadeStatistics; }
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
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    bool m_diffuse = false, m_water = false, m_cascadeDiffuse = false;
    Statistics m_diffuseStatistics, m_reflectionStatistics;
    cascades::Statistics m_cascadeStatistics;
    VkDescriptorSetLayout m_cascadeSceneLayout = VK_NULL_HANDLE;
    // Destruction order matters: reflections retain references to the scene.
    std::unique_ptr<LiveScene> m_scene;
    std::unique_ptr<Reflections> m_reflections;
    std::unique_ptr<cascades::RadianceCascades> m_cascades;
  };
}
