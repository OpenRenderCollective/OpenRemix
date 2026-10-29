#include "upscale/fsr_backends.h"
#include "upscale/sdk_backend.h"
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace remix::upscale {
namespace {
void checkFfx(FfxErrorCode r) {
  if (r != FFX_OK)
    throw std::runtime_error("FSR 3.1 error " + std::to_string(r));
}
FfxResource resource(Input input, VkFormat format, VkExtent2D extent, const wchar_t *name) {
  FfxResourceDescription desc{};
  desc.type = FFX_RESOURCE_TYPE_TEXTURE2D;
  desc.format = ffxGetSurfaceFormatVK(format);
  desc.width = extent.width;
  desc.height = extent.height;
  desc.depth = 1;
  desc.mipCount = 1;
  desc.usage = FFX_RESOURCE_USAGE_UAV;
  return ffxGetResourceVK(reinterpret_cast<void *>(input.image), desc, name, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}
struct LinkedUpscaler : FsrUpscaler::Impl {
  struct alignas(32) ScratchBlock {
    uint64_t words[4]{};
  };
  std::vector<ScratchBlock> scratch;
  FfxFsr3UpscalerContext context{};
  VkDeviceContext device{};
  bool created{};
  ~LinkedUpscaler() override {
    if (created)
      ffxFsr3UpscalerContextDestroy(&context);
  }
  void create(VkPhysicalDevice physical, VkDevice device, VkExtent2D render, VkExtent2D display) override;
  void dispatch(VkCommandBuffer command, const FsrDispatch &frame) override;
  uint64_t memoryBytes() const override;
  bool sharedImages() const override { return true; }
};
} // namespace

std::unique_ptr<FsrUpscaler::Impl> linkedUpscaler() { return std::make_unique<LinkedUpscaler>(); }

void LinkedUpscaler::create(VkPhysicalDevice physical, VkDevice device, VkExtent2D render, VkExtent2D display) {
  auto &g = *this;
  // What the embedded portable shaders declare.
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceFeatures(physical, &features);
  VkPhysicalDeviceSubgroupProperties subgroup{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup};
  vkGetPhysicalDeviceProperties2(physical, &properties);
  if (!features.shaderStorageImageWriteWithoutFormat || !(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
      !(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT))
    throw std::runtime_error("FSR requires compute subgroup quads and unformatted image writes");
  // The pinned SDK's ffxGetDeviceVK returns a process-global mutable context.
  // Own the same adapter representation per renderer while the active context
  // evaluates and its replacement initializes on the compiler worker.
  g.device = {device, physical, sdkDeviceProc};
  FfxFsr3UpscalerContextDescription desc{};
  desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
  desc.maxRenderSize = {render.width, render.height};
  desc.maxUpscaleSize = {display.width, display.height};
  g.scratch.resize((ffxGetScratchMemorySizeVK(physical, 1) + 31) / 32);
  checkFfx(ffxGetInterfaceVK(&desc.backendInterface, reinterpret_cast<FfxDevice>(&g.device), g.scratch.data(),
                             g.scratch.size() * 32, 1));
  usePortableShaders(desc.backendInterface);
  ContextAllocations allocations;
  checkFfx(ffxFsr3UpscalerContextCreate(&g.context, &desc));
  g.created = true;
  allocations.commit();
  g.provider = "FSR 3.1.4";
}

void LinkedUpscaler::dispatch(VkCommandBuffer command, const FsrDispatch &frame) {
  FfxFsr3UpscalerDispatchDescription desc{};
  desc.commandList = ffxGetCommandListVK(command);
  desc.color = resource(frame.color, VK_FORMAT_R16G16B16A16_SFLOAT, frame.render, L"Denoised HDR");
  desc.depth = resource(frame.depth, VK_FORMAT_R32_SFLOAT, frame.render, L"Device depth");
  desc.motionVectors = resource(frame.motion, VK_FORMAT_R32G32B32A32_SFLOAT, frame.render, L"Unjittered pixel motion");
  desc.reactive = resource(frame.reactive, VK_FORMAT_R8_UNORM, frame.render, L"Reactive");
  desc.transparencyAndComposition = resource(frame.composition, VK_FORMAT_R8_UNORM, frame.render, L"Composition");
  desc.dilatedDepth = resource(frame.dilatedDepth, VK_FORMAT_R32_SFLOAT, frame.render, L"Dilated depth");
  desc.dilatedMotionVectors = resource(frame.dilatedMotion, VK_FORMAT_R16G16_SFLOAT, frame.render, L"Dilated motion");
  desc.reconstructedPrevNearestDepth = resource(frame.previousDepth, VK_FORMAT_R32_UINT, frame.render, L"Previous depth");
  desc.output = resource(frame.output, VK_FORMAT_R16G16B16A16_SFLOAT, frame.display, L"Upscaled HDR");
  desc.jitterOffset = {frame.jitter[0], frame.jitter[1]};
  desc.motionVectorScale = {1, 1};
  desc.renderSize = {frame.render.width, frame.render.height};
  desc.upscaleSize = {frame.display.width, frame.display.height};
  desc.enableSharpening = frame.sharpness > 0;
  desc.sharpness = frame.sharpness;
  desc.frameTimeDelta = frame.milliseconds;
  desc.preExposure = 1;
  desc.reset = frame.reset;
  desc.cameraNear = frame.cameraNear;
  desc.cameraFar = frame.cameraFar;
  desc.cameraFovAngleVertical = frame.fieldOfView;
  desc.viewSpaceToMetersFactor = 1;
  checkFfx(ffxFsr3UpscalerContextDispatch(&context, &desc));
}

uint64_t LinkedUpscaler::memoryBytes() const {
  FfxEffectMemoryUsage usage{};
  return created && ffxFsr3UpscalerContextGetGpuMemoryUsage(const_cast<FfxFsr3UpscalerContext *>(&context),
                                                             &usage) == FFX_OK
             ? usage.totalUsageInBytes
             : 0;
}
} // namespace remix::upscale
