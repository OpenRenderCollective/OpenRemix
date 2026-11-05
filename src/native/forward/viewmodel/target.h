#pragma once
#include "forward/resources.h"
#include "scene/bake_scene.h"
#include <array>
#include <functional>
#include <memory>

namespace remix::forward::viewmodel {
  // Independent depth preserves both weapon ordering and the world's depth /
  // reflection guides. Images are allocated only after a viewmodel is present.
  class Target {
  public:
    ~Target();
    void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent,
                    VkSampleCountFlagBits samples);
    void ensure();
    void record(VkCommandBuffer command, const std::function<void()> &draw);
    VkRenderPass pass() const { return m_pass; }
    VkImageView color() const { return m_resolved ? m_resolved->view : m_color->view; }
    uint64_t bytes() const;

  private:
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    VkSampleCountFlagBits m_samples = VK_SAMPLE_COUNT_1_BIT;
    VkRenderPass m_pass = VK_NULL_HANDLE;
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
    std::unique_ptr<Image> m_color, m_depth, m_resolved;
  };
  class SunShadow {
  public:
    ~SunShadow();
    void initialize(VkPhysicalDevice physical, VkDevice device, VkRenderPass pass, uint32_t resolution);
    // Layer 0 is the static fallback; layer 1 contains moving/self shadows.
    // Reuse the fallback only for the same frozen scene and exact projection.
    // A new target starts invalid; moving/self shadows always update.
    bool updateStatic(const std::shared_ptr<const scene::BakeScene> &source, const scene::Matrix &projection);
    void record(VkCommandBuffer command, bool refreshStatic, const std::function<void(uint32_t)> &draw);
    VkImageView view() const { return m_depth.view; }
    uint64_t bytes() const { return m_depth.bytes; }

  private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkRenderPass m_pass = VK_NULL_HANDLE;
    std::array<VkFramebuffer, 2> m_framebuffers{};
    std::array<VkImageView, 2> m_layers{};
    uint32_t m_resolution = 0;
    Image m_depth;
    std::weak_ptr<const scene::BakeScene> m_staticSource;
    scene::Matrix m_staticProjection{};
    bool m_staticValid = false, m_staticHasSource = false;
  };
} // namespace remix::forward::viewmodel
