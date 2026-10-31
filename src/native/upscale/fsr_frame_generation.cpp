#include "upscale/fsr_backends.h"
#include "upscale/sdk_backend.h"
#include <bit>
#include <cstdlib>
#include <stdexcept>
#include <vector>
// After <bit>, which the SDK's utility header uses without including.
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>

namespace remix::upscale {
namespace {
void check(VkResult result) {
  if (result != VK_SUCCESS)
    throw result;
}
void checkFfx(FfxErrorCode result, const char *step) {
  if (result != FFX_OK)
    throw std::runtime_error(std::string("FSR 3.1 frame generation: ") + step + " failed (" +
                             std::to_string(int(result)) + ")");
}
// The 10-bit swapchain formats hold HDR10 (ST.2084 PQ) frames
// (runtime/hdr_output.h); the 8-bit ones gamma encoded SDR.
FfxBackbufferTransferFunction transferFunction(VkFormat format) {
  return format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || format == VK_FORMAT_A2R10G10B10_UNORM_PACK32
             ? FFX_BACKBUFFER_TRANSFER_FUNCTION_PQ
             : FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
}
// The formats of the resources the effects share with their caller
// (ffxFrameInterpolationGetSharedResourceDescriptions,
// ffxOpticalflowGetSharedResourceDescriptions).
VkFormat sharedFormat(FfxSurfaceFormat format) {
  switch (format) {
  case FFX_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
  case FFX_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
  case FFX_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
  case FFX_SURFACE_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
  default: throw std::runtime_error("FSR 3.1 frame generation: unexpected shared resource format");
  }
}
// An image an effect writes for another to read: the SDK has its caller own
// these. The backend makes its own views of it at each use.
struct SharedImage {
  VkDevice device{};
  VkImage image{};
  VkDeviceMemory memory{};
  FfxResourceDescription description{};
  const wchar_t *name = L"";
  SharedImage() = default;
  SharedImage(const SharedImage &) = delete;
  SharedImage &operator=(const SharedImage &) = delete;
  ~SharedImage() {
    if (image)
      vkDestroyImage(device, image, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
  }
  void create(VkPhysicalDevice physical, VkDevice owner, const FfxCreateResourceDescription &request) {
    device = owner;
    description = request.resourceDescription;
    name = request.name;
    // Storage and sampled for the passes; transfer destination for the
    // backend's clear jobs (the reconstructed depth every frame, the scene
    // change data at a reset).
    const VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = sharedFormat(description.format),
                                 .extent = {description.width, description.height, 1},
                                 .mipLevels = 1,
                                 .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    check(vkCreateImage(device, &info, nullptr, &image));
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    uint32_t type = 0;
    for (; type < properties.memoryTypeCount; ++type)
      if ((requirements.memoryTypeBits & (1u << type)) &&
          (properties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        break;
    if (type == properties.memoryTypeCount)
      throw VK_ERROR_FEATURE_NOT_PRESENT;
    const VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                          .allocationSize = requirements.size,
                                          .memoryTypeIndex = type};
    check(vkAllocateMemory(device, &allocation, nullptr, &memory));
    check(vkBindImageMemory(device, image, memory, 0));
  }
  // Kept in GENERAL between the SDK's calls, which return it there.
  FfxResource resource() const {
    return ffxGetResourceVK(reinterpret_cast<void *>(image), description, name, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
  }
};
// A caller's image as the SDK takes it. `written`: the SDK also makes a
// storage view of it and expects GENERAL; otherwise SHADER_READ_ONLY_OPTIMAL.
// The backend moves a registered image between layouts as its passes need and
// returns it to this one when the call ends (UnregisterResourcesVK).
FfxResource resource(Input input, VkFormat format, VkExtent2D extent, const wchar_t *name, bool written = false) {
  if (!input.image)
    return {};
  FfxResourceDescription description{};
  description.type = FFX_RESOURCE_TYPE_TEXTURE2D;
  description.format = ffxGetSurfaceFormatVK(format);
  description.width = extent.width;
  description.height = extent.height;
  description.depth = 1;
  description.mipCount = 1;
  description.usage = written ? FFX_RESOURCE_USAGE_UAV : FFX_RESOURCE_USAGE_READ_ONLY;
  return ffxGetResourceVK(reinterpret_cast<void *>(input.image), description, name,
                          written ? FFX_RESOURCE_STATE_UNORDERED_ACCESS : FFX_RESOURCE_STATE_COMPUTE_READ);
}
struct LinkedFrameGeneration : FsrFrameGeneration::Impl {
  VkDevice device{};
  VkExtent2D display{}, render{};
  VkFormat format{};
  bool depthInverted{};
  struct alignas(32) ScratchBlock {
    uint64_t words[4]{};
  };
  // One backend, shared by the two effects as in AMD's own frame generation
  // (its "Fi" interface): the backend's memory, and the device it refers to.
  std::vector<ScratchBlock> scratch;
  VkDeviceContext sdkDevice{};
  FfxOpticalflowContext opticalFlow{};
  FfxFrameInterpolationContext interpolation{};
  bool created = false;
  // Written by the interpolation's prepare stage and read by its dispatch in
  // the same frame, so one set suffices (AMD alternates two only to let the
  // dispatch run asynchronously).
  SharedImage dilatedDepth, dilatedMotion, previousDepth;
  // Written by optical flow, read by the interpolation.
  SharedImage flowVector, sceneChange;
  bool sharedReady = false; // the shared images have left UNDEFINED
  bool reset = true;        // no usable previous frame
  uint64_t frame = 0;
  // Views of the images above and of the caller's that the backend has let
  // go of. Last: freed after ~Impl has destroyed the contexts and before the
  // shared images.
  std::unique_ptr<DeferredViews> views;
  LinkedFrameGeneration() { provider = "FSR 3.1.4"; }
  ~LinkedFrameGeneration() override {
    if (created) {
      ffxFrameInterpolationContextDestroy(&interpolation);
      ffxOpticalflowContextDestroy(&opticalFlow);
    }
  }
  void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D display, VkFormat format, VkExtent2D render,
                  bool depthInverted) override;
  void discontinuity() override { reset = true; }
  void record(VkCommandBuffer command, Input backbuffer, Input depth, Input motion, Input hudless, Input output,
              const FrameGenerationCamera &camera, float frameMilliseconds) override;
};
} // namespace

std::unique_ptr<FsrFrameGeneration::Impl> linkedFrameGeneration() { return std::make_unique<LinkedFrameGeneration>(); }

void LinkedFrameGeneration::initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D display,
                                       VkFormat format, VkExtent2D render, bool depthInverted) {
  auto &g = *this;
  g.device = device;
  g.display = display;
  g.render = render;
  g.format = format;
  g.depthInverted = depthInverted;
  // The interpolation keeps the previous frame in the backbuffer's format, as
  // a storage image, and accepts few formats (GetFormatPrecisionGroup in
  // ffx_frameinterpolation.cpp). The frame is passed as sRGB-encoded (record).
  if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM)
    throw std::runtime_error("FSR 3.1 frame generation needs an 8-bit RGBA or BGRA swapchain");
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceFeatures(physical, &features);
  VkPhysicalDeviceSubgroupProperties subgroup{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                         .pNext = &subgroup};
  vkGetPhysicalDeviceProperties2(physical, &properties);
  // What the embedded passes declare: the output is read and written without
  // a format (it has the swapchain's), the pyramids use subgroup quads and
  // the optical flow search subgroup arithmetic.
  const VkSubgroupFeatureFlags operations = VK_SUBGROUP_FEATURE_QUAD_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
  if (!features.shaderStorageImageReadWithoutFormat || !features.shaderStorageImageWriteWithoutFormat ||
      !features.shaderStorageImageExtendedFormats || !(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
      (subgroup.supportedOperations & operations) != operations)
    throw std::runtime_error(
        "FSR 3.1 frame generation requires compute subgroup quads and arithmetic and unformatted image access");
  for (auto required : {format, VK_FORMAT_R8_UINT, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R16G16_SINT,
                        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SFLOAT,
                        VK_FORMAT_R16G16B16A16_SFLOAT}) {
    VkFormatProperties support{};
    vkGetPhysicalDeviceFormatProperties(physical, required, &support);
    const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((support.optimalTilingFeatures & needed) != needed)
      throw std::runtime_error("FSR 3.1 frame generation: sampled/storage image formats unavailable");
  }

  g.views = std::make_unique<DeferredViews>(device, gpuFrameCount);
  // As for the upscaler: the pinned SDK's ffxGetDeviceVK returns a
  // process-global context, so the object owns the adapter description.
  g.sdkDevice = {device, physical, sdkDeviceProc};
  const size_t contexts = FFX_OPTICALFLOW_CONTEXT_COUNT + FFX_FRAMEINTERPOLATION_CONTEXT_COUNT;
  g.scratch.resize((ffxGetScratchMemorySizeVK(physical, contexts) + 31) / 32);
  FfxInterface backend{};
  checkFfx(ffxGetInterfaceVK(&backend, reinterpret_cast<FfxDevice>(&g.sdkDevice), g.scratch.data(),
                             g.scratch.size() * 32, contexts),
           "backend creation");
  usePortableShaders(backend);

  // Reclaims what either context made if the other, or a shared image, fails.
  ContextAllocations allocations;
  // Optical flow works on the displayed frame, in blocks of 8 pixels.
  FfxOpticalflowContextDescription flow{};
  flow.backendInterface = backend;
  flow.resolution = {display.width, display.height};
  checkFfx(ffxOpticalflowContextCreate(&g.opticalFlow, &flow), "optical flow creation");

  FfxFrameInterpolationContextDescription interpolation{};
  interpolation.backendInterface = backend;
  // Not set: DEPTH_INFINITE (the projections have a far plane; depthRange),
  // DISPLAY_RESOLUTION_MOTION_VECTORS and JITTER_MOTION_VECTORS (motion is at
  // render resolution and unjittered), HDR_COLOR_INPUT (the frame is the
  // encoded SDR image), ASYNC_SUPPORT (prepare and dispatch are recorded
  // together). These and DEPTH_INVERTED select the shader permutation
  // (getPipelinePermutationFlags), of which only these two are embedded.
  interpolation.flags = depthInverted ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED : 0;
  interpolation.maxRenderSize = {render.width, render.height};
  interpolation.displaySize = {display.width, display.height};
  interpolation.backBufferFormat = ffxGetSurfaceFormatVK(format);
  // The HUD-less frame, when there is one, replaces the backbuffer as the
  // interpolation source and is what is kept for the next frame; both have
  // the one format, which the dispatch asserts.
  interpolation.previousInterpolationSourceFormat = interpolation.backBufferFormat;
  checkFfx(ffxFrameInterpolationContextCreate(&g.interpolation, &interpolation), "frame interpolation creation");

  FfxFrameInterpolationSharedResourceDescriptions interpolationShared{};
  checkFfx(ffxFrameInterpolationGetSharedResourceDescriptions(&g.interpolation, &interpolationShared),
           "shared resource query");
  FfxOpticalflowSharedResourceDescriptions flowShared{};
  checkFfx(ffxOpticalflowGetSharedResourceDescriptions(&g.opticalFlow, &flowShared), "shared resource query");
  g.dilatedDepth.create(physical, device, interpolationShared.dilatedDepth);
  g.dilatedMotion.create(physical, device, interpolationShared.dilatedMotionVectors);
  g.previousDepth.create(physical, device, interpolationShared.reconstructedPrevNearestDepth);
  g.flowVector.create(physical, device, flowShared.opticalFlowVector);
  g.sceneChange.create(physical, device, flowShared.opticalFlowSCD);
  allocations.commit();
  g.created = true;
}

void LinkedFrameGeneration::record(VkCommandBuffer command, Input backbuffer, Input depth, Input motion, Input hudless,
                                   Input output, const FrameGenerationCamera &camera, float frameMilliseconds) {
  auto &g = *this;
  // The recording frame's slot is free: so is every view retired
  // gpuFrameCount frames ago.
  g.views->advance();
  DeferredViews::Scope deferred(*g.views);
  if (!g.sharedReady) {
    for (auto *shared : {&g.dilatedDepth, &g.dilatedMotion, &g.previousDepth, &g.flowVector, &g.sceneChange}) {
      const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                         .image = shared->image,
                                         .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &barrier);
    }
    g.sharedReady = true;
  }
  // Both effects also reset at their first dispatch; frame interpolation then
  // writes the current frame instead of an interpolated one
  // (computeFrameinterpolation, FrameIndexSinceLastReset() == 0).
  const bool reset = camera.reset || g.reset;
  const auto list = ffxGetCommandListVK(command);
  const auto color = resource(backbuffer, g.format, g.display, L"Presented frame");
  const auto withoutHud = resource(hudless, g.format, g.display, L"Frame without HUD");
  // The setup maps device depth 0 and 1 to these distances and only swaps
  // them for inverted depth after ordering them itself
  // (setupDeviceDepthToViewSpaceDepthParams), so either order gives the same
  // constants; the shaders do not read the two values. Passed as for the
  // upscaler, whose names follow the depth value: "near" is depth 0.
  const float depthZero = camera.depthInverted ? camera.farPlane : camera.nearPlane;
  const float depthOne = camera.depthInverted ? camera.nearPlane : camera.farPlane;

  // 1. Prepare: dilates depth and motion to the nearest depth of each 3x3
  // neighbourhood and reprojects that depth into the previous frame.
  FfxFrameInterpolationPrepareDescription prepare{};
  prepare.commandList = list;
  prepare.renderSize = {g.render.width, g.render.height};
  // Read only when motion vectors are at display resolution or jittered
  // (ComputeHrPosFromLrPos, MotionVectorJitterCancellation); neither here.
  prepare.jitterOffset = {0, 0};
  // The stage divides the scaled vector by the render size
  // (ffxFrameInterpolationPrepare) and adds the result to a pixel's UV to find
  // it in the previous frame (computeGameFieldMvs): render pixels towards the
  // previous frame need no scale, as for the upscaler.
  prepare.motionVectorScale = {1, 1};
  prepare.frameTimeDelta = frameMilliseconds;
  prepare.cameraNear = depthZero;
  prepare.cameraFar = depthOne;
  prepare.viewSpaceToMetersFactor = 1;
  prepare.cameraFovAngleVertical = camera.fieldOfView;
  prepare.depth = resource(depth, VK_FORMAT_R32_SFLOAT, g.render, L"Device depth");
  prepare.motionVectors = resource(motion, VK_FORMAT_R32G32_SFLOAT, g.render, L"Unjittered pixel motion");
  prepare.frameID = g.frame;
  prepare.dilatedDepth = g.dilatedDepth.resource();
  prepare.dilatedMotionVectors = g.dilatedMotion.resource();
  prepare.reconstructedPrevDepth = g.previousDepth.resource();
  for (int i = 0; i < 3; ++i) {
    prepare.cameraPosition[i] = camera.position[i];
    prepare.cameraUp[i] = camera.up[i];
    prepare.cameraRight[i] = camera.right[i];
    prepare.cameraForward[i] = camera.forward[i];
  }
  checkFfx(ffxFrameInterpolationPrepare(&g.interpolation, &prepare), "prepare");

  // 2. Optical flow between the previous and the current frame, which it
  // keeps as luminance pyramids of its own. Of the same image the
  // interpolation moves: the HUD-less frame when there is one.
  FfxOpticalflowDispatchDescription flow{};
  flow.commandList = list;
  flow.color = hudless.image ? withoutHud : color;
  flow.opticalFlowVector = g.flowVector.resource();
  flow.opticalFlowSCD = g.sceneChange.resource();
  flow.reset = reset;
  // The images hold the frame as displayed. The luminance range is read for
  // the HDR transfer functions only: PQ's whole range.
  flow.backbufferTransferFunction = transferFunction(g.format);
  const float maxLuminance = flow.backbufferTransferFunction == FFX_BACKBUFFER_TRANSFER_FUNCTION_PQ ? 10000.f : 0.f;
  flow.minMaxLuminance = {0, maxLuminance};
  checkFfx(ffxOpticalflowContextDispatch(&g.opticalFlow, &flow), "optical flow");

  // 3. Interpolation: writes the frame halfway to `output` and copies its
  // source (the HUD-less frame, else the backbuffer) to the image it keeps as
  // the next frame's previous one.
  FfxFrameInterpolationDispatchDescription dispatch{};
  dispatch.commandList = list;
  dispatch.displaySize = {g.display.width, g.display.height};
  dispatch.renderSize = {g.render.width, g.render.height};
  dispatch.currentBackBuffer = color;
  // With it, the interface is put back from the backbuffer wherever the two
  // differ (computeInpainting), instead of being moved with the scene.
  dispatch.currentBackBuffer_HUDLess = withoutHud;
  dispatch.output = resource(output, g.format, g.display, L"Generated frame", true);
  dispatch.interpolationRect = {0, 0, int32_t(g.display.width), int32_t(g.display.height)};
  dispatch.opticalFlowVector = g.flowVector.resource();
  dispatch.opticalFlowSceneChangeDetection = g.sceneChange.resource();
  dispatch.opticalFlowBufferSize = {g.flowVector.description.width, g.flowVector.description.height};
  // Optical flow vectors are in display pixels, one per block of 8
  // (opticalFlowBlockSize in ffx_opticalflow.cpp's dispatch); the scale takes
  // them to UV, and a positive one tells the dispatch the two images are given.
  dispatch.opticalFlowScale = {1.f / float(g.display.width), 1.f / float(g.display.height)};
  dispatch.opticalFlowBlockSize = 8;
  dispatch.cameraNear = depthZero;
  dispatch.cameraFar = depthOne;
  dispatch.cameraFovAngleVertical = camera.fieldOfView;
  dispatch.viewSpaceToMetersFactor = 1;
  dispatch.frameTimeDelta = frameMilliseconds;
  dispatch.reset = reset;
  // Diagnostic (REMIX_FSR_FG_DEBUG=<FfxFrameInterpolationDispatchFlags>): 2
  // marks the generated frame's top rows red at a reset and blue below them
  // while optical flow reports a scene change.
  static const uint32_t debugFlags = [] {
    const char *value = std::getenv("REMIX_FSR_FG_DEBUG");
    return value ? uint32_t(std::strtoul(value, nullptr, 10)) : 0u;
  }();
  dispatch.flags = debugFlags;
  dispatch.backBufferTransferFunction = transferFunction(g.format);
  dispatch.minMaxLuminance[0] = 0;
  dispatch.minMaxLuminance[1] = dispatch.backBufferTransferFunction == FFX_BACKBUFFER_TRANSFER_FUNCTION_PQ ? 10000.f : 0.f;
  // A step other than one resets the interpolation; discontinuities are
  // passed as `reset` instead, so the counter only ever steps.
  dispatch.frameID = g.frame;
  dispatch.dilatedDepth = g.dilatedDepth.resource();
  dispatch.dilatedMotionVectors = g.dilatedMotion.resource();
  dispatch.reconstructedPrevDepth = g.previousDepth.resource();
  checkFfx(ffxFrameInterpolationDispatch(&g.interpolation, &dispatch), "interpolation");
  ++g.frame;
  g.reset = false;
}
} // namespace remix::upscale
