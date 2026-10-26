#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

namespace remix {
  // HDR10 output: while Windows has HDR on for the window's display, the
  // swapchain is BT.2020 ST.2084 PQ (A2B10G10R10, 10 bits) and the frame's
  // passes draw into a float back buffer instead, gamma 2.2 encoded with 1.0
  // at paper white (the traced scene tone mapped relative to paper white up
  // to the peak, displayColor). encode() decodes it, converts it to BT.2020 and encodes PQ
  // with 1.0 at paper white into the swapchain image.
  //
  // The UI (the HUD layer and the developer UI) is drawn as the game draws it,
  // SDR over the scene, at its own brightness: scaleUi() brackets it. Under a
  // pure 2.2 curve, linearizing a UI colour, scaling it by UI / paper white
  // nits and encoding it again is a scale of the encoded colour by
  // uiScale(); dividing the back buffer by it before the UI and multiplying
  // after leaves the scene as it was and scales what the UI drew over it (by
  // alpha blending, adding or multiplying).
  struct HdrNits {
    float peak = 1000, paperWhite = 203, ui = 203;
    // The accepted ranges; the peak is at least paper white (the tone curve's
    // peak relative to it at least 1).
    HdrNits clamped() const {
      HdrNits c;
      c.paperWhite = std::clamp(paperWhite, 80.f, 1000.f);
      c.peak = std::clamp(peak, c.paperWhite, 10000.f);
      c.ui = std::clamp(ui, 80.f, 1000.f);
      return c;
    }
    // The scale of gamma 2.2 encoded UI colours that puts UI white at the UI
    // brightness: (UI / paper white nits)^(1 / 2.2).
    float uiScale() const {
      const auto c = clamped();
      return std::pow(c.ui / c.paperWhite, 1.f / 2.2f);
    }
  };

  class HdrOutput {
  public:
    static constexpr VkFormat backBufferFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    // The swapchain formats taken for HDR10, in order of preference.
    static constexpr VkFormat swapchainFormats[] = {VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                                    VK_FORMAT_A2R10G10B10_UNORM_PACK32};
    static bool hdr10Format(VkFormat format) {
      return format == swapchainFormats[0] || format == swapchainFormats[1];
    }

    HdrOutput() = default;
    ~HdrOutput() { destroy(); }
    HdrOutput(const HdrOutput &) = delete;
    HdrOutput &operator=(const HdrOutput &) = delete;

    // `slots` back buffers (one per frame in flight; GENERAL layout, sampled,
    // copied to and from) with framebuffers of `mainPass` beside `depth`, the
    // UI scale's pipeline in `mainPass`, and the encode pass into each of
    // `swapchainViews`. Throws VkResult.
    void create(VkPhysicalDevice physical, VkDevice device, VkFormat swapchainFormat, VkExtent2D extent,
                const std::vector<VkImageView> &swapchainViews, uint32_t slots, VkRenderPass mainPass,
                VkImageView depth);
    void destroy(); // the device has completed the work that used it
    bool active() const { return m_device != VK_NULL_HANDLE; }

    VkImage backBuffer(uint32_t slot) const { return m_backBuffers[slot].image; }
    VkFramebuffer framebuffer(uint32_t slot) const { return m_backBuffers[slot].framebuffer; }

    // At the start of a frame: the back buffer of `slot` enters GENERAL the
    // first time it is used (its passes load or clear it in GENERAL).
    void prepare(VkCommandBuffer command, uint32_t slot);
    // Inside a pass of `mainPass`: the back buffer times `factor` (1 /
    // uiScale() before UI draws, uiScale() after them).
    void scaleUi(VkCommandBuffer command, float factor);
    // Outside a render pass: the back buffer of `slot` (GENERAL, written by
    // the frame's passes) to swapchain image `imageIndex` (left in
    // PRESENT_SRC), PQ with 1.0 at `paperWhiteNits`.
    void encode(VkCommandBuffer command, uint32_t slot, uint32_t imageIndex, float paperWhiteNits);

  private:
    struct BackBuffer {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      VkImageView view = VK_NULL_HANDLE;
      VkFramebuffer framebuffer = VK_NULL_HANDLE;
      VkDescriptorSet set = VK_NULL_HANDLE;
      bool initialized = false; // in GENERAL
    };
    VkDevice m_device = VK_NULL_HANDLE;
    VkExtent2D m_extent{};
    std::vector<BackBuffer> m_backBuffers;
    std::vector<VkFramebuffer> m_swapchainFramebuffers;
    VkRenderPass m_pass = VK_NULL_HANDLE;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE, m_uiLayout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE, m_uiPipeline = VK_NULL_HANDLE;
  };
} // namespace remix
