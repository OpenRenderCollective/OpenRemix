#pragma once
#include "pathtrace/options.h"
#include "scene/temporal.h"
#include "upscale/renderer.h"
#include <memory>

namespace remix::postfx {
class State;
enum class MeterMode { MaterialNormalized, SceneRadiance };
struct MeterInputs {
  VkImageLayout colorLayout = VK_IMAGE_LAYOUT_GENERAL;
  // SceneRadiance adds this optional linear reflection image to unpremultiplied
  // HDR color. MaterialNormalized uses the renderer's own albedo guide instead.
  VkDescriptorImageInfo auxiliary{};
};
struct Statistics {
  float exposure = 1, target = 1, limit = 0, milliseconds = 0;
  uint64_t memoryBytes = 0, meterFrames = 0, bloomFrames = 0;
  bool adapting = false;
};
// Shared by both compositors. The device cache owns the immutable layout.
VkDescriptorSetLayout compositeLayout(VkDevice device);
class Renderer {
public:
  Renderer();
  ~Renderer();
  // Called only on the preparation worker. State survives target replacement.
  void initialize(VkPhysicalDevice physical, VkDevice device, uint32_t family,
                  VkExtent2D display, VkExtent2D render, bool meter, bool bloom,
                  std::shared_ptr<State> state = {}, MeterMode mode = MeterMode::MaterialNormalized);
  std::shared_ptr<State> state() const;
  bool supports(VkExtent2D display, VkExtent2D render, bool meter,
                bool bloom) const;
  void selectFrame(uint32_t slot);
  void begin(VkCommandBuffer command);
  VkImageView materialGuide() const;
  upscale::Input materialInput() const;
  upscale::Input bloomOutput() const;
  bool needsGuide() const;
  void record(VkCommandBuffer command, upscale::Input hdr,
              const TraceOptions &options, scene::TemporalEpoch epoch,
              bool changed, bool bypass, float seconds = -1, uint64_t generation = 0,
              MeterInputs inputs = {});
  void freeze();
  VkDescriptorSet bindings() const;
  Statistics statistics() const;

private:
  struct Gpu;
  std::unique_ptr<Gpu> m_gpu;
};
} // namespace remix::postfx
