#pragma once
#include "platform/gpu_frames.h"
#include "upscale/options.h"
#include "scene/display.h"
#include "scene/types.h"
#include <memory>
#include <string>
#include <vulkan/vulkan.h>
namespace remix::upscale {
enum class Backend : uint32_t { Native, Fsr31, DlssSr, DlssRr, Xess };
const char *name(Backend backend);
class Ngx;
struct Input {
  VkImage image{};
  VkImageView view{};
};
struct ReconstructionGuides {
  Input diffuse, specular, normalRoughness, hitDistance;
};
// What frame generation takes from the upscaler: the device depth and motion
// vectors (render pixels) it prepared for this frame, both in GENERAL.
struct FrameGenerationInputs {
  Input depth, motion;
  VkExtent2D render{};
  bool depthInverted{}; // nearer is larger (the FSR path's depth)
  bool valid{};
};
// The camera of a frame as the frame generation features take it. Matrices
// without jitter, in scene::Matrix storage (see NgxFrameGeneration::record).
// `nearPlane` and `farPlane`: the distances of device depth 0 and 1 before any
// reversal; `fieldOfView`: vertical, in radians.
struct FrameGenerationCamera {
  scene::Matrix viewToClip{}, clipToView{}, clipToPrevClip{}, prevClipToClip{};
  float position[3]{}, up[3]{}, right[3]{}, forward[3]{};
  float nearPlane{}, farPlane{}, fieldOfView{}, aspect{};
  bool depthInverted{}, reset{};
};
struct Statistics {
  Backend backend{};
  Mode requested{};
  bool srSupported{}, rrSupported{}, sdkAvailable{}, failed{};
  uint64_t failures{}, memoryBytes{};
  bool active{};
  VkExtent2D render{}, display{};
  uint64_t frames{}, resets{};
  float milliseconds{};
  std::string failure;
  // FSR and XeSS: the provider evaluating, e.g. "FSR 4.1.2" (FsrUpscaler::provider).
  std::string provider;
  // Ray Reconstruction: the CNN model's library is packaged / loaded, the
  // NGX preset created (1-6, A-F) and why the requested model is not loaded.
  bool rrCnnPackaged{}, rrCnn{};
  uint32_t rrPreset{};
  std::string rrNote;
};
bool supportsCamera(const scene::Matrix &projection);
VkExtent2D renderSize(VkExtent2D display, uint32_t quality);
// XeSS's presets (native AA, ultra quality plus, ultra quality, quality,
// balanced, performance, ultra performance): the XeSS-SR developer guide's
// ratios 1, 1.3, 1.5, 1.7, 2, 2.3 and 3, rounded up.
VkExtent2D xessRenderSize(VkExtent2D display, uint32_t quality);
void jitter(float *xy, uint64_t frame, VkExtent2D render, VkExtent2D display);
class Renderer {
public:
  Renderer();
  ~Renderer();
  void initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily, VkExtent2D render,
                  VkExtent2D display, Backend backend = Backend::Fsr31, std::shared_ptr<Ngx> ngx = {},
                  uint32_t quality = 1, uint32_t rrPreset = 0);
  void configureComposite(VkRenderPass pass, VkImageView stableDepth, VkRenderPass tonemapPass = VK_NULL_HANDLE);
  void selectFrame(uint32_t slot);
  // display: displayColor's (pathtrace/shaders/display.glsl).
  void composite(VkCommandBuffer command, const scene::DisplayOutput &display, VkDescriptorSet post);
  Input output() const;
  FrameGenerationInputs frameGenerationInputs() const;
  Input preparedDepth() const;
  Input preparedMotion() const;
  void record(VkCommandBuffer command, Input color, Input depth, Input motion, Input transmission,
              const scene::Matrix &projection, const float *jitter, float milliseconds, float sharpness,
              bool reset, const ReconstructionGuides &guides = {}, const scene::Matrix &view = {});
  Statistics statistics() const;
  bool finishInitialization(VkQueue queue);

private:
  struct Gpu;
  std::unique_ptr<Gpu> m_gpu;
};
} // namespace remix::upscale
