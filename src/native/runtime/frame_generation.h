#pragma once
#include "pathtrace/renderer.h"
#include "upscale/fsr_frame_generation.h"
#include "upscale/ngx.h"
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace remix {
// Frame generation ("rtx.native.frameGeneration"): generated frames between
// each pair of rendered frames, by DLSS Frame Generation (one to three) or by
// FSR frame generation (one; "rtx.native.frameGenerationBackend").
//
// The frame is rendered into its swapchain image as without it. Afterwards the
// finished image (scene, HUD and developer UI) is copied into an image the
// feature can sample, the feature interpolates between it and the previous one
// into a second image, guided by the upscaler's depth and motion vectors, and
// that is copied into a further swapchain image, once per generated frame. The
// runtime presents the generated images and then the rendered one, spread
// evenly over the frame (presentation.cpp).
//
// The HUD: the frame as it was before the UI layer was drawn (captureHudless)
// tells the feature which pixels are interface, which it then keeps from the
// rendered frames instead of moving them with the scene.
//
// Owns one backend's feature and its images; sized to one swapchain extent,
// format and render extent (matches()).
class FrameGeneration {
public:
  enum class Backend { Dlss, Fsr31 };
  static constexpr uint32_t maxGenerated = 3; // 4x
  // Generated frames per rendered frame a backend can make at most, before
  // what the GPU's feature allows.
  static constexpr uint32_t maxGeneratedBy(Backend backend) { return backend == Backend::Fsr31 ? 1 : maxGenerated; }
  // `depthInverted`: of the depth record() will be given; FSR compiles it in
  // (DLSS is told with each frame). An Fsr31 object compiles pipelines: make
  // it on the pipeline service's worker.
  FrameGeneration(Backend backend, std::shared_ptr<upscale::Ngx> ngx, VkPhysicalDevice physical, VkDevice device,
                  uint32_t queueFamily, VkExtent2D display, VkFormat swapchainFormat, VkExtent2D render,
                  bool depthInverted);
  ~FrameGeneration();
  FrameGeneration(const FrameGeneration &) = delete;
  FrameGeneration &operator=(const FrameGeneration &) = delete;
  Backend backend() const { return m_backendKind; }
  // For status lines: "DLSS", or the FSR provider (e.g. "FSR 4.0.2").
  std::string name() const { return m_fsr ? m_fsr->provider() : "DLSS"; }
  bool matches(Backend backend, VkExtent2D display, VkFormat swapchainFormat, VkExtent2D render,
               bool depthInverted) const;
  // Submits the feature's creation commands once and reports whether they
  // have completed (the caller owns the queue).
  bool ready(VkQueue queue);
  // Between render passes, before the UI layer: `real` (PRESENT_SRC, as a
  // pass leaves it) holds the frame without its HUD. Optional, once a frame.
  void captureHudless(VkCommandBuffer command, VkImage real);
  // `real` holds the finished frame; `generated` are the `count` further
  // acquired images (undefined contents), in the order they are shown. All are
  // left in PRESENT_SRC. Throws when the feature refuses an evaluation; the
  // images are then as duplicate() leaves them.
  void record(VkCommandBuffer command, VkImage real, const VkImage *generated, uint32_t count,
              const PathTracer::FrameGenerationInputs &inputs);
  // No interpolation this frame: the `generated` images become copies of `real`.
  void duplicate(VkCommandBuffer command, VkImage real, const VkImage *generated, uint32_t count);

private:
  struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };
  Target create(VkPhysicalDevice physical, VkImageUsageFlags usage) const;
  void destroy(Target &target) const;

  Backend m_backendKind = Backend::Dlss;
  VkDevice m_device = VK_NULL_HANDLE;
  VkExtent2D m_display{}, m_render{};
  VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED, m_format = VK_FORMAT_UNDEFINED;
  bool m_depthInverted = false; // Fsr31: what its shaders were chosen for
  Target m_backbuffer, m_interpolated, m_hudless;
  // The feature of m_backendKind; the other stays empty.
  std::unique_ptr<upscale::NgxFrameGeneration> m_dlss;
  std::unique_ptr<upscale::FsrFrameGeneration> m_fsr;
  bool m_first = true;           // no previous frame yet: the feature is told to reset
  bool m_hudlessCaptured = false; // this frame (cleared by record and duplicate)
};
// The swapchain images of a frame's generated frames, in the order shown.
struct GeneratedImages {
  uint32_t index[FrameGeneration::maxGenerated] = {};
  uint32_t count = 0;
};
} // namespace remix
