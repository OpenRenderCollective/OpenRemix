#include "upscale/fsr_backends.h"
#include "upscale/ffx_library.h"
#include <SDL3/SDL_log.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>

namespace remix::upscale {
namespace {
// Images the caller keeps in GENERAL, which the Vulkan backend calls
// UNORDERED_ACCESS, both before and after a dispatch.
FfxApiResource resource(Input input, VkFormat format, VkExtent2D extent) {
  FfxApiResource out{};
  if (!input.image)
    return out;
  out.resource = reinterpret_cast<void *>(input.image);
  out.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
  out.description.format = ffxApiGetSurfaceFormatVK(format);
  out.description.width = extent.width;
  out.description.height = extent.height;
  out.description.depth = 1;
  out.description.mipCount = 1;
  out.description.usage = FFX_API_RESOURCE_USAGE_UAV;
  out.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
  return out;
}
void message(uint32_t type, const wchar_t *text) {
  std::string narrow;
  for (const wchar_t *c = text; c && *c; ++c)
    narrow += *c < 128 ? char(*c) : '?';
  SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning",
              narrow.c_str());
}
struct FfxUpscaler : FsrUpscaler::Impl {
  ffxContext context{};
  ~FfxUpscaler() override {
    if (context)
      ffx::api().destroyContext(&context, nullptr);
  }
  void create(VkPhysicalDevice physical, VkDevice device, VkExtent2D render, VkExtent2D display) override;
  void dispatch(VkCommandBuffer command, const FsrDispatch &frame) override;
  uint64_t memoryBytes() const override;
  bool sharedImages() const override { return false; }
};
} // namespace

std::unique_ptr<FsrUpscaler::Impl> ffxUpscaler() { return std::make_unique<FfxUpscaler>(); }

void FfxUpscaler::create(VkPhysicalDevice physical, VkDevice device, VkExtent2D render, VkExtent2D display) {
  const auto &api = ffx::api();
  ffxCreateBackendVKDesc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.deviceContext = {device, physical, vkGetDeviceProcAddr};
  ffxOverrideVersion overrideStorage{};
  backend.header.pNext =
      ffx::versionOverride(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &backend.deviceContext, overrideStorage);
  ffxCreateContextDescUpscaleVersion version{};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  version.header.pNext = &backend.header;
  version.version = FFX_UPSCALER_VERSION;
  ffxCreateContextDescUpscale desc{};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  desc.header.pNext = &version.header;
  desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
  desc.maxRenderSize = {render.width, render.height};
  desc.maxUpscaleSize = {display.width, display.height};
  desc.fpMessage = message;
  ffx::check(api.createContext(&context, &desc.header, nullptr), "FSR context creation");
  provider = ffx::provider(context, "FSR");
}

void FfxUpscaler::dispatch(VkCommandBuffer command, const FsrDispatch &frame) {
  ffxDispatchDescUpscale desc{};
  desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  desc.commandList = command;
  desc.color = resource(frame.color, VK_FORMAT_R16G16B16A16_SFLOAT, frame.render);
  desc.depth = resource(frame.depth, VK_FORMAT_R32_SFLOAT, frame.render);
  desc.motionVectors = resource(frame.motion, VK_FORMAT_R32G32B32A32_SFLOAT, frame.render);
  desc.reactive = resource(frame.reactive, VK_FORMAT_R8_UNORM, frame.render);
  desc.transparencyAndComposition = resource(frame.composition, VK_FORMAT_R8_UNORM, frame.render);
  desc.output = resource(frame.output, VK_FORMAT_R16G16B16A16_SFLOAT, frame.display);
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
  ffx::check(ffx::api().dispatch(&context, &desc.header), "FSR dispatch");
}

uint64_t FfxUpscaler::memoryBytes() const {
  if (!context)
    return 0;
  FfxApiEffectMemoryUsage usage{};
  ffxQueryDescUpscaleGetGPUMemoryUsage query{};
  query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
  query.gpuMemoryUsageUpscaler = &usage;
  auto queried = context;
  return ffx::api().query(&queried, &query.header) == FFX_API_RETURN_OK ? usage.totalUsageInBytes : 0;
}
} // namespace remix::upscale
