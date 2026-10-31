#pragma once
#include "upscale/renderer.h"
#include <mutex>
#include <vector>

namespace remix::upscale {
struct NgxCapabilities {
  bool sdk{}, sr{}, rr{}, initializationComplete{};
  std::string reason;
  // DLSS Frame Generation (NVSDK_NGX_Feature_FrameGeneration). `fgMultiFrameMax`:
  // generated frames per rendered frame the feature supports (1: 2x only).
  bool fg{};
  uint32_t fgMultiFrameMax{};
  std::string fgReason;
  // Ray Reconstruction's CNN model: its library is packaged; it is loaded.
  bool rrCnnPackaged{}, rrCnn{};
};
// Device lifetime; features hold a reference and must be released before shutdown.
class Ngx {
public:
  Ngx();
  ~Ngx();
  std::vector<std::string> instanceExtensions();
  std::vector<std::string> deviceExtensions(VkInstance instance, VkPhysicalDevice physical);
  void setDevice(VkInstance instance, VkPhysicalDevice physical,
                 VkDevice device);
  void initializeDevice();
  void initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
  NgxCapabilities capabilities() const;
  VkExtent2D renderSize(VkExtent2D display, uint32_t quality, bool rr);
  // Loads Ray Reconstruction's CNN model (an older release's library) or the
  // current transformer models. NGX restarts for it, so only while no feature
  // exists. False (with `reason`) leaves the loaded model.
  bool useRayReconstructionModel(bool cnn, std::string &reason);
  VkInstance instanceHandle() const { return instance; }
  VkDevice deviceHandle() const { return device; }

private:
  friend class NgxFeature;
  friend class NgxFrameGeneration;
  struct Impl;
  // (Re)initializes NGX and its capabilities (DLSS builds).
  void start(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
  std::unique_ptr<Impl> m;
  mutable std::mutex capabilitiesMutex;
  NgxCapabilities publishedCapabilities;
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  bool deviceAssigned = false;
};
class NgxFeature {
public:
  NgxFeature();
  ~NgxFeature();
  void initialize(std::shared_ptr<Ngx> context, uint32_t family, VkExtent2D render, VkExtent2D display,
                  uint32_t quality, bool rr, uint32_t rrPreset = 0);
  bool finishInitialization(VkQueue queue);
  uint64_t memoryBytes() const;
  void record(VkCommandBuffer command, Input color, Input depth, Input motion, Input output, Input exposure,
              const ReconstructionGuides &guides, const scene::Matrix &view, const scene::Matrix &projection,
              const float *jitter, float milliseconds, bool reset);

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
// DLSS Frame Generation (NVSDK_NGX_Feature_FrameGeneration): one generated
// frame between the previous and the current backbuffer.
class NgxFrameGeneration {
public:
  NgxFrameGeneration();
  ~NgxFrameGeneration();
  // `format`: of the backbuffer and output images; `render`: of depth and motion.
  void initialize(std::shared_ptr<Ngx> context, uint32_t family, VkExtent2D display, VkFormat format,
                  VkExtent2D render);
  bool finishInitialization(VkQueue queue);
  // Inputs in SHADER_READ_ONLY_OPTIMAL, `output` in GENERAL. `hudless`: the
  // backbuffer before its interface was drawn (no image: not provided).
  // `generated` frames are made between the two rendered ones; `index` (from
  // one) is the one to write.
  void record(VkCommandBuffer command, Input backbuffer, Input depth, Input motion, Input hudless, Input output,
              const FrameGenerationCamera &camera, uint32_t generated, uint32_t index);

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
} // namespace remix::upscale
