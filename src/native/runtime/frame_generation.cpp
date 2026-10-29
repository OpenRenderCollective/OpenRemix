#include "runtime/frame_generation.h"
#include <cstdlib>
#include "platform/gpu_timers.h"
#include "upscale/camera.h"
#include <cmath>
#include <stdexcept>

namespace remix {
namespace {
void check(VkResult result) {
  if (result != VK_SUCCESS)
    throw result;
}
// The UNORM twin of an sRGB swapchain format: the images hold the encoded
// frame as it is displayed, which is what the feature interpolates, and raw
// copies between the twins keep the bits.
VkFormat encoded(VkFormat format) {
  switch (format) {
  case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
  case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
  default: return format;
  }
}
void transition(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to) {
  const VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                     .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                     .oldLayout = from,
                                     .newLayout = to,
                                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                     .image = image,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);
}
void copy(VkCommandBuffer command, VkImage source, VkImage destination, VkExtent2D extent) {
  const VkImageCopy region{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .extent = {extent.width, extent.height, 1}};
  vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}
} // namespace

FrameGeneration::FrameGeneration(Backend backend, std::shared_ptr<upscale::Ngx> ngx, VkPhysicalDevice physical,
                                 VkDevice device, uint32_t queueFamily, VkExtent2D display,
                                 VkFormat swapchainFormat, VkExtent2D render, bool depthInverted)
    : m_backendKind(backend), m_device(device), m_display(display), m_render(render),
      m_swapchainFormat(swapchainFormat), m_format(encoded(swapchainFormat)), m_depthInverted(depthInverted) {
  try {
    // Transfer source: the frames are copied to the swapchain, and FSR copies
    // the frame it interpolated from to the image it keeps for the next one.
    const VkImageUsageFlags sampled =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    m_backbuffer = create(physical, sampled);
    m_hudless = create(physical, sampled);
    m_interpolated = create(physical, sampled | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (backend == Backend::Fsr31) {
      m_fsr = std::make_unique<upscale::FsrFrameGeneration>();
      m_fsr->initialize(physical, device, display, m_format, render, depthInverted);
    } else {
      m_dlss = std::make_unique<upscale::NgxFrameGeneration>();
      m_dlss->initialize(std::move(ngx), queueFamily, display, m_format, render);
    }
  } catch (...) {
    destroy(m_backbuffer);
    destroy(m_hudless);
    destroy(m_interpolated);
    throw;
  }
}

FrameGeneration::~FrameGeneration() {
  // Before the images: FSR's backend holds views of them until it goes.
  m_fsr.reset();
  destroy(m_backbuffer);
  destroy(m_hudless);
  destroy(m_interpolated);
}

FrameGeneration::Target FrameGeneration::create(VkPhysicalDevice physical, VkImageUsageFlags usage) const {
  Target target;
  try {
    const VkImageCreateInfo image{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .imageType = VK_IMAGE_TYPE_2D,
                                  .format = m_format,
                                  .extent = {m_display.width, m_display.height, 1},
                                  .mipLevels = 1,
                                  .arrayLayers = 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = usage};
    check(vkCreateImage(m_device, &image, nullptr, &target.image));
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_device, target.image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t type = 0;
    for (; type < memory.memoryTypeCount; ++type)
      if ((requirements.memoryTypeBits & (1u << type)) &&
          (memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        break;
    if (type == memory.memoryTypeCount)
      throw VK_ERROR_FEATURE_NOT_PRESENT;
    const VkMemoryAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                          .allocationSize = requirements.size,
                                          .memoryTypeIndex = type};
    check(vkAllocateMemory(m_device, &allocation, nullptr, &target.memory));
    check(vkBindImageMemory(m_device, target.image, target.memory, 0));
    const VkImageViewCreateInfo view{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                     .image = target.image,
                                     .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                     .format = m_format,
                                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(m_device, &view, nullptr, &target.view));
  } catch (...) {
    destroy(target);
    throw;
  }
  return target;
}

void FrameGeneration::destroy(Target &target) const {
  if (target.view)
    vkDestroyImageView(m_device, target.view, nullptr);
  if (target.image)
    vkDestroyImage(m_device, target.image, nullptr);
  if (target.memory)
    vkFreeMemory(m_device, target.memory, nullptr);
  target = {};
}

bool FrameGeneration::matches(Backend backend, VkExtent2D display, VkFormat swapchainFormat, VkExtent2D render,
                              bool depthInverted) const {
  return backend == m_backendKind && display.width == m_display.width && display.height == m_display.height &&
         swapchainFormat == m_swapchainFormat && render.width == m_render.width &&
         render.height == m_render.height && (!m_fsr || depthInverted == m_depthInverted);
}

bool FrameGeneration::ready(VkQueue queue) {
  return m_fsr ? m_fsr->finishInitialization(queue) : m_dlss->finishInitialization(queue);
}

void FrameGeneration::captureHudless(VkCommandBuffer command, VkImage real) {
  gpu::GpuScope scope(command, "Frame generation: frame without HUD", m_display);
  transition(command, real, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  transition(command, m_hudless.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copy(command, real, m_hudless.image, m_display);
  transition(command, real, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  transition(command, m_hudless.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  m_hudlessCaptured = true;
}

void FrameGeneration::duplicate(VkCommandBuffer command, VkImage real, const VkImage *generated, uint32_t count) {
  m_hudlessCaptured = false;
  // FSR keeps the previous frame itself and is not shown this one.
  if (m_fsr)
    m_fsr->discontinuity();
  transition(command, real, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  for (uint32_t i = 0; i < count; ++i) {
    transition(command, generated[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy(command, real, generated[i], m_display);
    transition(command, generated[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  transition(command, real, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

void FrameGeneration::record(VkCommandBuffer command, VkImage real, const VkImage *generated, uint32_t count,
                             const PathTracer::FrameGenerationInputs &inputs) {
  // FSR's contexts are built for one render size and depth direction. The
  // upscaler can have been replaced since the runtime last compared them
  // (matches()), which it does again before the next frame: show this one
  // without interpolation.
  if (m_fsr && (inputs.images.depthInverted != m_depthInverted || inputs.images.render.width != m_render.width ||
                inputs.images.render.height != m_render.height)) {
    duplicate(command, real, generated, count);
    return;
  }
  gpu::GpuScope scope(command, m_fsr ? "FSR frame generation" : "DLSS Frame Generation", m_display);
  // Diagnostic (REMIX_FG_NO_HUDLESS=1): the features are not given the frame
  // without HUD.
  static const bool withoutHudless = [] {
    const char *value = std::getenv("REMIX_FG_NO_HUDLESS");
    return value && value[0] == '1';
  }();
  const bool hudless = m_hudlessCaptured && !withoutHudless;
  m_hudlessCaptured = false;
  // The finished frame, as the feature's backbuffer; the swapchain image is
  // not created with the usage the feature needs to sample it.
  transition(command, real, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  transition(command, m_backbuffer.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  copy(command, real, m_backbuffer.image, m_display);
  transition(command, real, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

  // Matrices: column vectors in column-major storage, which is byte for byte
  // the row-vector, row-major form NGX takes (as for Ray Reconstruction). FSR
  // takes none of them: only the planes, the field of view and the basis.
  upscale::FrameGenerationCamera camera{};
  const auto &view = inputs.view, &projection = inputs.projection;
  const auto clipToView = scene::inverse(projection), viewToWorld = scene::inverse(view);
  const auto clipToPrevClip = scene::multiply(
      inputs.previousProjection, scene::multiply(inputs.previousView, scene::multiply(viewToWorld, clipToView)));
  camera.viewToClip = projection;
  camera.clipToView = clipToView;
  camera.clipToPrevClip = clipToPrevClip;
  camera.prevClipToClip = scene::inverse(clipToPrevClip);
  // World-space basis and position: the columns of the inverse view. The
  // camera looks along the view axis the projection maps to positive w.
  const float forward = projection[11] < 0 ? -1.f : 1.f;
  for (int i = 0; i < 3; ++i) {
    camera.right[i] = viewToWorld[i];
    camera.up[i] = viewToWorld[4 + i];
    camera.forward[i] = forward * viewToWorld[8 + i];
    camera.position[i] = viewToWorld[12 + i];
  }
  const auto range = upscale::depthRange(projection);
  camera.nearPlane = range.nearPlane;
  camera.farPlane = range.farPlane;
  camera.fieldOfView = 2 * std::atan(1 / std::abs(projection[5]));
  camera.aspect = std::abs(projection[5] / projection[0]);
  camera.depthInverted = inputs.images.depthInverted;
  camera.reset = inputs.reset || m_first;

  // Both features sample their inputs in SHADER_READ_ONLY_OPTIMAL and write
  // their output in GENERAL; the upscaler's depth and motion are kept in
  // GENERAL.
  auto inputsLayout = [&](bool begin) {
    for (auto image : {inputs.images.depth.image, inputs.images.motion.image})
      transition(command, image, begin ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 begin ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL);
  };
  transition(command, m_backbuffer.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  inputsLayout(true);
  std::string failure;
  uint32_t done = 0;
  for (; done < count && failure.empty(); ++done) {
    transition(command, m_interpolated.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    const upscale::Input backbuffer{m_backbuffer.image, m_backbuffer.view},
        output{m_interpolated.image, m_interpolated.view},
        withoutHud = hudless ? upscale::Input{m_hudless.image, m_hudless.view} : upscale::Input{};
    try {
      // FSR interpolates halfway only and advances its history with each
      // call: it is given a single generated frame (maxGeneratedBy).
      if (m_fsr)
        m_fsr->record(command, backbuffer, inputs.images.depth, inputs.images.motion, withoutHud, output, camera,
                      inputs.frameMilliseconds);
      else
        m_dlss->record(command, backbuffer, inputs.images.depth, inputs.images.motion, withoutHud, output, camera,
                       count, done + 1);
    } catch (const std::exception &e) {
      failure = e.what();
      break;
    }
    transition(command, m_interpolated.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(command, generated[done], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy(command, m_interpolated.image, generated[done], m_display);
    transition(command, generated[done], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  inputsLayout(false);
  if (failure.empty()) {
    m_first = false;
    return;
  }
  // A refused evaluation: the images not yet written show the rendered frame.
  transition(command, m_backbuffer.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  for (; done < count; ++done) {
    transition(command, generated[done], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy(command, m_backbuffer.image, generated[done], m_display);
    transition(command, generated[done], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  throw std::runtime_error(failure);
}
} // namespace remix
