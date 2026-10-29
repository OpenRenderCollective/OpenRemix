#include "upscale/fsr_backends.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace remix::upscale {
namespace {
// Test switch (Windows builds made with -Dfsr_linked=true): REMIX_FSR_LINKED=1
// runs the linked FSR 3.1.4 that Linux and macOS run instead of the DLLs.
[[maybe_unused]] bool linkedRequested() {
  static const bool requested = [] {
    const char *value = std::getenv("REMIX_FSR_LINKED");
    return value && value[0] == '1';
  }();
  return requested;
}
std::unique_ptr<FsrUpscaler::Impl> upscaler() {
#if REMIX_FSR_LINKED && REMIX_FSR_FFX
  return linkedRequested() ? linkedUpscaler() : ffxUpscaler();
#elif REMIX_FSR_FFX
  return ffxUpscaler();
#else
  return linkedUpscaler();
#endif
}
std::unique_ptr<FsrFrameGeneration::Impl> frameGeneration() {
#if REMIX_FSR_LINKED && REMIX_FSR_FFX
  return linkedRequested() ? linkedFrameGeneration() : ffxFrameGeneration();
#elif REMIX_FSR_FFX
  return ffxFrameGeneration();
#else
  return linkedFrameGeneration();
#endif
}
// FSR's quality ratios and Halton jitter (ffxFsr3UpscalerGetJitterOffset in
// ffx_fsr3upscaler.cpp), which every provider shares: computed here so that a
// render size needs no DLL.
constexpr float ratios[] = {1.f, 1.5f, 1.7f, 2.f, 3.f};
float halton(int32_t index, int32_t base) {
  float f = 1, result = 0;
  for (int32_t current = index; current > 0;) {
    f /= float(base);
    result += f * float(current % base);
    current = int32_t(std::floor(float(current) / float(base)));
  }
  return result;
}
} // namespace

VkExtent2D renderSize(VkExtent2D display, uint32_t quality) {
  const float ratio = ratios[std::min(quality, 4u)];
  return {std::max(uint32_t(float(display.width) / ratio), 1u), std::max(uint32_t(float(display.height) / ratio), 1u)};
}
void jitter(float *xy, uint64_t frame, VkExtent2D render, VkExtent2D display) {
  const float scale = float(display.width) / float(render.width);
  const int32_t phase = std::max(int32_t(8.f * std::pow(scale, 2.f)), 1);
  const int32_t index = int32_t(frame % uint64_t(phase)) + 1;
  xy[0] = halton(index, 2) - .5f;
  xy[1] = halton(index, 3) - .5f;
}

FsrUpscaler::FsrUpscaler() : m(upscaler()) {}
FsrUpscaler::~FsrUpscaler() = default;
void FsrUpscaler::create(VkPhysicalDevice physical, VkDevice device, VkExtent2D render, VkExtent2D display) {
  m->create(physical, device, render, display);
}
void FsrUpscaler::dispatch(VkCommandBuffer command, const FsrDispatch &frame) { m->dispatch(command, frame); }
uint64_t FsrUpscaler::memoryBytes() const { return m->memoryBytes(); }
bool FsrUpscaler::sharedImages() const { return m->sharedImages(); }
const std::string &FsrUpscaler::provider() const { return m->provider; }

FsrFrameGeneration::FsrFrameGeneration() : m(frameGeneration()) {}
FsrFrameGeneration::~FsrFrameGeneration() = default;
void FsrFrameGeneration::initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D display, VkFormat format,
                                    VkExtent2D render, bool depthInverted) {
  m->initialize(physical, device, display, format, render, depthInverted);
}
bool FsrFrameGeneration::finishInitialization(VkQueue) { return true; }
void FsrFrameGeneration::discontinuity() { m->discontinuity(); }
void FsrFrameGeneration::record(VkCommandBuffer command, Input backbuffer, Input depth, Input motion, Input hudless,
                                Input output, const FrameGenerationCamera &camera, float frameMilliseconds) {
  m->record(command, backbuffer, depth, motion, hudless, output, camera, frameMilliseconds);
}
const std::string &FsrFrameGeneration::provider() const { return m->provider; }
} // namespace remix::upscale
