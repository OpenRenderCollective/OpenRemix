#include "upscale/fsr_backends.h"
#include "upscale/ffx_library.h"
#include <cstdlib>
#include <stdexcept>
#include <ffx_framegeneration.h>
#include <vk/ffx_api_vk.h>

namespace remix::upscale {
namespace {
// The 10-bit swapchain formats hold HDR10 (ST.2084 PQ) frames
// (runtime/hdr_output.h); the 8-bit ones gamma encoded SDR.
uint32_t transferFunction(VkFormat format) {
  return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ? FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ
                                                      : FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
}
// A caller's image as the API takes it. `written`: in GENERAL, else in
// SHADER_READ_ONLY_OPTIMAL; the backend returns it to that layout.
FfxApiResource resource(Input input, VkFormat format, VkExtent2D extent, bool written = false) {
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
  out.description.usage = written ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
  out.state = written ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
  return out;
}
struct FfxFrameGeneration : FsrFrameGeneration::Impl {
  VkExtent2D display{}, render{};
  VkFormat format{};
  ffxContext context{};
  bool reset = true; // no usable previous frame
  uint64_t frame = 0;
  ~FfxFrameGeneration() override {
    if (context)
      ffx::api().destroyContext(&context, nullptr);
  }
  void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D display, VkFormat format, VkExtent2D render,
                  bool depthInverted) override;
  void discontinuity() override { reset = true; }
  void record(VkCommandBuffer command, Input backbuffer, Input depth, Input motion, Input hudless, Input output,
              const FrameGenerationCamera &camera, float frameMilliseconds) override;
};
} // namespace

std::unique_ptr<FsrFrameGeneration::Impl> ffxFrameGeneration() { return std::make_unique<FfxFrameGeneration>(); }

void FfxFrameGeneration::initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D display,
                                    VkFormat format, VkExtent2D render, bool depthInverted) {
  auto &g = *this;
  g.display = display;
  g.render = render;
  g.format = format;
  if (ffxApiGetSurfaceFormatVK(format) == FFX_API_SURFACE_FORMAT_UNKNOWN)
    throw std::runtime_error("FSR frame generation does not take this swapchain format");
  const auto &api = ffx::api();
  ffxCreateBackendVKDesc backend{};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
  backend.deviceContext = {device, physical, vkGetDeviceProcAddr};
  ffxOverrideVersion overrideStorage{};
  backend.header.pNext =
      ffx::versionOverride(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, &backend.deviceContext, overrideStorage);
  ffxCreateContextDescFrameGenerationVersion version{};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
  version.header.pNext = &backend.header;
  version.version = FFX_FRAMEGENERATION_VERSION;
  ffxCreateContextDescFrameGeneration desc{};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
  desc.header.pNext = &version.header;
  // Not set: DEPTH_INFINITE (the projections have a far plane; depthRange),
  // DISPLAY_RESOLUTION_MOTION_VECTORS and MOTION_VECTORS_JITTER_CANCELLATION
  // (motion is at render resolution and unjittered), HIGH_DYNAMIC_RANGE (the
  // frame is the encoded image), ASYNC_WORKLOAD_SUPPORT (prepare and dispatch
  // are recorded together on the presenting queue).
  desc.flags = depthInverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0;
  desc.displaySize = {display.width, display.height};
  desc.maxRenderSize = {render.width, render.height};
  desc.backBufferFormat = ffxApiGetSurfaceFormatVK(format);
  ffx::check(api.createContext(&g.context, &desc.header, nullptr), "FSR frame generation context creation");
  g.provider = ffx::provider(g.context, "FSR");
}

void FfxFrameGeneration::record(VkCommandBuffer command, Input backbuffer, Input depth, Input motion, Input hudless,
                                Input output, const FrameGenerationCamera &camera, float frameMilliseconds) {
  auto &g = *this;
  const auto &api = ffx::api();
  const bool reset = camera.reset || g.reset;
  const FfxApiRect2D rect{0, 0, int32_t(g.display.width), int32_t(g.display.height)};
  // Diagnostic (REMIX_FSR_FG_DEBUG=<FfxApiDispatchFramegenerationFlags>): 1
  // draws tear lines, 2 marks the generated frame at a reset.
  static const uint32_t debugFlags = [] {
    const char *value = std::getenv("REMIX_FSR_FG_DEBUG");
    return value ? uint32_t(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  // The runtime presents the frames itself: no swapchain context is told
  // (NO_SWAPCHAIN_CONTEXT_NOTIFY), and the generated frame is dispatched here
  // rather than from a present callback.
  const uint32_t flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY | debugFlags;

  // 1. Configure, once a frame with the frame's ID. With a HUD-less frame the
  // interface is put back from the backbuffer instead of being moved.
  ffxConfigureDescFrameGeneration configure{};
  configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
  configure.frameGenerationEnabled = true;
  configure.HUDLessColor = resource(hudless, g.format, g.display);
  configure.flags = flags;
  configure.generationRect = rect;
  configure.frameID = g.frame;
  ffx::check(api.configure(&g.context, &configure.header), "FSR frame generation configuration");

  // 2. Prepare: dilates depth and motion and reprojects depth into the
  // previous frame. The distances are passed as for the upscaler, whose names
  // follow the depth value: "near" is depth 0.
  ffxDispatchDescFrameGenerationPrepareV2 prepare{};
  prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
  prepare.frameID = g.frame;
  prepare.flags = flags;
  prepare.commandList = command;
  prepare.renderSize = {g.render.width, g.render.height};
  // Motion is unjittered, in render pixels towards the previous frame.
  prepare.jitterOffset = {0, 0};
  prepare.motionVectorScale = {1, 1};
  prepare.frameTimeDelta = frameMilliseconds;
  prepare.reset = reset;
  prepare.cameraNear = camera.depthInverted ? camera.farPlane : camera.nearPlane;
  prepare.cameraFar = camera.depthInverted ? camera.nearPlane : camera.farPlane;
  prepare.cameraFovAngleVertical = camera.fieldOfView;
  prepare.viewSpaceToMetersFactor = 1;
  prepare.depth = resource(depth, VK_FORMAT_R32_SFLOAT, g.render);
  prepare.motionVectors = resource(motion, VK_FORMAT_R32G32_SFLOAT, g.render);
  for (int i = 0; i < 3; ++i) {
    prepare.cameraPosition[i] = camera.position[i];
    prepare.cameraUp[i] = camera.up[i];
    prepare.cameraRight[i] = camera.right[i];
    prepare.cameraForward[i] = camera.forward[i];
  }
  ffx::check(api.dispatch(&g.context, &prepare.header), "FSR frame generation prepare");

  // 3. Generation: optical flow between the previous and this frame, then
  // the frame halfway written to `output`.
  ffxDispatchDescFrameGeneration dispatch{};
  dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
  dispatch.commandList = command;
  dispatch.presentColor = resource(backbuffer, g.format, g.display);
  dispatch.outputs[0] = resource(output, g.format, g.display, true);
  dispatch.numGeneratedFrames = 1;
  dispatch.reset = reset;
  dispatch.backbufferTransferFunction = transferFunction(g.format);
  // Read for the HDR transfer functions only: PQ's whole range.
  dispatch.minMaxLuminance[0] = 0;
  dispatch.minMaxLuminance[1] =
      dispatch.backbufferTransferFunction == FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ ? 10000.f : 0.f;
  dispatch.generationRect = rect;
  // A step other than one resets the generation; discontinuities are passed
  // as `reset` instead, so the counter only ever steps.
  dispatch.frameID = g.frame;
  ffx::check(api.dispatch(&g.context, &dispatch.header), "FSR frame generation");
  ++g.frame;
  g.reset = false;
}
} // namespace remix::upscale
