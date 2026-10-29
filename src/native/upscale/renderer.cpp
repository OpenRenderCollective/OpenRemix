#include "upscale/renderer.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include "postfx/renderer.h"
#include "upscale/camera.h"
#include "upscale/fsr.h"
#include "upscale/ngx.h"
#include "upscale/xess.h"
#include "upscale/shaders/embedded.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace remix::upscale {
namespace {
void check(VkResult r) {
  if (r != VK_SUCCESS)
    throw r;
}
struct Image {
  VkDevice device{};
  VkImage image{};
  VkImageView view{};
  VkDeviceMemory memory{};
  VkExtent2D extent{};
  VkFormat format{};
  ~Image() {
    if (view)
      vkDestroyImageView(device, view, nullptr);
    if (image)
      vkDestroyImage(device, image, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
  }
};
void barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier b{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr,
                       0, nullptr);
}
Input input(const std::unique_ptr<Image> &image) {
  return image ? Input{image->image, image->view} : Input{};
}
} // namespace
struct Renderer::Gpu {
  VkDevice device{};
  VkPhysicalDevice physical{};
  VkPhysicalDeviceMemoryProperties memory{};
  VkExtent2D render{}, display{};
  std::unique_ptr<FsrUpscaler> fsr;
  std::unique_ptr<NgxFeature> ngx;
#if REMIX_HAS_XESS
  std::unique_ptr<XessUpscaler> xess;
#endif
  std::unique_ptr<Image> motion, exposure;
  bool initialized{};
  std::unique_ptr<Image> depth, reactive, composition, output, dilatedDepth,
      dilatedMotion, previousDepth;
  VkDescriptorSetLayout setLayout{};
  VkDescriptorPool pool{};
  VkDescriptorSet set{}, sets[gpuFrameCount]{};
  VkPipelineLayout layout{};
  VkPipeline prepare{};
  VkDescriptorSetLayout compositeSetLayout{}, postSetLayout{};
  VkDescriptorPool compositePool{};
  VkDescriptorSet compositeSet{};
  VkPipelineLayout compositeLayout{};
  VkPipeline compositePipeline{};
  VkQueryPool timestamps{}, frameTimestamps[gpuFrameCount]{};
  bool timingPending[gpuFrameCount]{};
  uint32_t frameIndex = 0;
  uint32_t timestampBits{};
  float timestampPeriod{};
  Statistics stats;
  VkPipeline tonemapCompositePipeline = VK_NULL_HANDLE;
  ~Gpu() {
    ngx.reset();
    fsr.reset();
#if REMIX_HAS_XESS
    xess.reset();
#endif
    if (tonemapCompositePipeline)
      pipeline::destroyPipeline(device, tonemapCompositePipeline, nullptr);
    if (compositePipeline)
      pipeline::destroyPipeline(device, compositePipeline, nullptr);
    if (compositeLayout)
      pipeline::destroyPipelineLayout(device, compositeLayout, nullptr);
    if (compositePool)
      vkDestroyDescriptorPool(device, compositePool, nullptr);
    if (postSetLayout) pipeline::destroyDescriptorSetLayout(device, postSetLayout, nullptr);
    if (compositeSetLayout)
      pipeline::destroyDescriptorSetLayout(device, compositeSetLayout, nullptr);
    if (prepare)
      pipeline::destroyPipeline(device, prepare, nullptr);
    if (layout)
      pipeline::destroyPipelineLayout(device, layout, nullptr);
    if (pool)
      vkDestroyDescriptorPool(device, pool, nullptr);
    if (setLayout)
      pipeline::destroyDescriptorSetLayout(device, setLayout, nullptr);
    for (auto query : frameTimestamps)
      if (query)
        vkDestroyQueryPool(device, query, nullptr);
  }
  std::unique_ptr<Image> image(VkFormat format, VkExtent2D extent) {
    auto out = std::make_unique<Image>();
    out->device = device;
    out->format = format;
    out->extent = extent;
    VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           .imageType = VK_IMAGE_TYPE_2D,
                           .format = format,
                           .extent = {extent.width, extent.height, 1},
                           .mipLevels = 1,
                           .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT,
                           .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateImage(device, &info, nullptr, &out->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, out->image, &req);
    uint32_t type = 0;
    for (; type < memory.memoryTypeCount; ++type)
      if ((req.memoryTypeBits & (1u << type)) &&
          (memory.memoryTypes[type].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        break;
    if (type == memory.memoryTypeCount)
      throw std::runtime_error("No FSR image memory type");
    stats.memoryBytes += req.size;
    VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = type};
    check(vkAllocateMemory(device, &alloc, nullptr, &out->memory));
    check(vkBindImageMemory(device, out->image, out->memory, 0));
    VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(device, &view, nullptr, &out->view));
    return out;
  }
};
const char *name(Backend backend) {
  switch (backend) {
  case Backend::Fsr31: return "FSR";
  case Backend::DlssSr: return "DLSS Super Resolution";
  case Backend::DlssRr: return "DLSS + Ray Reconstruction";
  case Backend::Xess: return "XeSS";
  default: return "Native";
  }
}
VkExtent2D xessRenderSize(VkExtent2D display, uint32_t quality) {
  constexpr float ratios[] = {1.f, 1.3f, 1.5f, 1.7f, 2.f, 2.3f, 3.f};
  const float ratio = ratios[std::min(quality, 6u)];
  // Rounded up: XeSS's smallest accepted input is the ratio's, rounded up.
  auto size = [&](uint32_t extent) {
    return std::clamp(uint32_t(std::ceil(float(extent) / ratio)), 1u, std::max(extent, 1u));
  };
  return {size(display.width), size(display.height)};
}
Renderer::Renderer() : m_gpu(std::make_unique<Gpu>()) {}
Renderer::~Renderer() = default;
bool supportsCamera(const scene::Matrix &projection) {
  if (std::abs(projection[11]) < .0001f || std::abs(projection[15]) > .0001f ||
      !projection[5])
    return false;
  const auto [nearPlane, farPlane] = depthRange(projection);
  return std::isfinite(nearPlane) && std::isfinite(farPlane) && nearPlane > 0 && farPlane > nearPlane;
}
void Renderer::initialize(VkPhysicalDevice physical, VkDevice device,
                          uint32_t queueFamily, VkExtent2D render,
                          VkExtent2D display, Backend backend, std::shared_ptr<Ngx> ngx, uint32_t quality,
                          uint32_t rrPreset) {
  auto &g = *m_gpu;
  g.stats.backend = backend;
  g.device = device;
  g.physical = physical;
  g.render = render;
  g.display = display;
  vkGetPhysicalDeviceMemoryProperties(physical, &g.memory);
  for (auto format :
       {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R32_UINT}) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &props);
    const auto required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((props.optimalTilingFeatures & required) != required)
      throw std::runtime_error("FSR sampled/storage image formats unavailable");
  }
  if (backend == Backend::Fsr31) {
    g.fsr = std::make_unique<FsrUpscaler>();
    g.fsr->create(physical, device, render, display);
    g.stats.provider = g.fsr->provider();
  } else if (backend == Backend::Xess) {
#if REMIX_HAS_XESS
    g.xess = std::make_unique<XessUpscaler>();
    g.xess->create(ngx ? ngx->instanceHandle() : VK_NULL_HANDLE, physical, device, render, display, quality);
    g.stats.provider = g.xess->provider();
#else
    throw std::runtime_error("XeSS is Windows only");
#endif
  } else {
    g.ngx = std::make_unique<NgxFeature>();
    g.ngx->initialize(std::move(ngx), queueFamily, render, display, quality, backend == Backend::DlssRr, rrPreset);
  }
  g.exposure = g.image(VK_FORMAT_R32_SFLOAT, {1,1});
  g.motion = g.image(VK_FORMAT_R32G32_SFLOAT, render);
  if (const char *fault = std::getenv("REMIX_NGX_FAULT");
      g.ngx && fault && std::string(fault) == "allocation")
    throw VK_ERROR_OUT_OF_DEVICE_MEMORY;
  g.depth = g.image(VK_FORMAT_R32_SFLOAT, render);
  g.reactive = g.image(VK_FORMAT_R8_UNORM, render);
  g.composition = g.image(VK_FORMAT_R8_UNORM, render);
  g.output = g.image(VK_FORMAT_R16G16B16A16_SFLOAT, display);
  // The linked SDK's intermediates, which the caller owns.
  if (g.fsr && g.fsr->sharedImages()) {
    g.dilatedDepth = g.image(VK_FORMAT_R32_SFLOAT, render);
    g.dilatedMotion = g.image(VK_FORMAT_R16G16_SFLOAT, render);
    g.previousDepth = g.image(VK_FORMAT_R32_UINT, render);
  }
  VkDescriptorSetLayoutBinding bindings[9]{};
  for (unsigned i = 0; i < 9; ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo si{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 9,
      .pBindings = bindings};
  check(
      pipeline::createDescriptorSetLayout(device, &si, nullptr, &g.setLayout));
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 9 * gpuFrameCount};
  VkDescriptorPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                .maxSets = gpuFrameCount,
                                .poolSizeCount = 1,
                                .pPoolSizes = &size};
  check(vkCreateDescriptorPool(device, &pi, nullptr, &g.pool));
  VkDescriptorSetAllocateInfo ai{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = g.pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &g.setLayout};
  for (auto &set : g.sets)
    check(vkAllocateDescriptorSets(device, &ai, &set));
  g.set = g.sets[0];
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(scene::Matrix) + 4};
  VkPipelineLayoutCreateInfo li{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &g.setLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push};
  check(pipeline::createPipelineLayout(device, &li, nullptr, &g.layout));
  VkShaderModuleCreateInfo mi{.sType =
                                  VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                              .codeSize = sizeof(compShader),
                              .pCode = compShader};
  VkShaderModule module{};
  check(pipeline::createShaderModule(device, &mi, nullptr, &module));
  VkComputePipelineCreateInfo ci{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = "main"},
      .layout = g.layout};
  auto result = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &ci,
                                                 nullptr, &g.prepare);
  pipeline::destroyShaderModule(device, module, nullptr);
  check(result);
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
  g.timestampBits = families.at(queueFamily).timestampValidBits;
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(physical, &props);
  g.timestampPeriod = props.limits.timestampPeriod;
  if (g.timestampBits) {
    VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                             .queryType = VK_QUERY_TYPE_TIMESTAMP,
                             .queryCount = 2};
    for (auto &query : g.frameTimestamps)
      check(vkCreateQueryPool(device, &qi, nullptr, &query));
    g.timestamps = g.frameTimestamps[0];
  }
  g.stats.memoryBytes += g.ngx ? g.ngx->memoryBytes() : g.fsr ? g.fsr->memoryBytes() : 0;
#if REMIX_HAS_XESS
  if (g.xess)
    g.stats.memoryBytes += g.xess->memoryBytes();
#endif
  g.stats.active = true;
  g.stats.render = render;
  g.stats.display = display;
}

void Renderer::selectFrame(uint32_t slot) {
  m_gpu->frameIndex = slot;
  m_gpu->set = m_gpu->sets[slot];
  m_gpu->timestamps = m_gpu->frameTimestamps[slot];
}
bool Renderer::finishInitialization(VkQueue queue) {
#if REMIX_HAS_XESS
  // libxess submits to the device's queue itself.
  if (m_gpu->xess)
    m_gpu->xess->finishInitialization();
#endif
  return !m_gpu->ngx || m_gpu->ngx->finishInitialization(queue);
}
Statistics Renderer::statistics() const { return m_gpu->stats; }
void Renderer::record(VkCommandBuffer command, Input color, Input depth,
                      Input motion, Input transmission,
                      const scene::Matrix &projection, const float *jitter,
                      float milliseconds, float sharpness, bool reset,
                      const ReconstructionGuides &guides, const scene::Matrix &view) {
  auto &g = *m_gpu;
  if (g.timestamps) {
    uint64_t times[2]{};
    if (g.timingPending[g.frameIndex] &&
        vkGetQueryPoolResults(g.device, g.timestamps, 0, 2, sizeof(times), times, 8,
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      uint64_t mask = g.timestampBits == 64
                          ? ~uint64_t(0)
                          : (uint64_t(1) << g.timestampBits) - 1;
      g.stats.milliseconds =
          float((times[1] - times[0]) & mask) * g.timestampPeriod / 1e6f;
    }
    vkCmdResetQueryPool(command, g.timestamps, 0, 2);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        g.timestamps, 0);
  }
  barrier(command);
  if (!g.initialized) {
    for (auto image :
         {g.depth.get(), g.reactive.get(), g.composition.get(), g.output.get(),
          g.dilatedDepth.get(), g.dilatedMotion.get(), g.previousDepth.get(), g.motion.get(), g.exposure.get()}) {
      if (!image)
        continue;
      VkImageMemoryBarrier b{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask =
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = image->image,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &b);
    }
  }
  if (!g.initialized) {
    barrier(command);
    const VkClearColorValue one{{1.f, 1.f, 1.f, 1.f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(command, g.exposure->image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
    barrier(command);
  }
  VkDescriptorImageInfo images[9]{};
  VkImageView views[] = {depth.view, transmission.view, g.depth->view,
                         g.reactive->view, g.composition->view, motion.view, g.motion->view, color.view, g.output->view};
  VkWriteDescriptorSet writes[9]{};
  for (unsigned i = 0; i < 9; ++i) {
    images[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = g.set,
                 .dstBinding = i,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .pImageInfo = &images[i]};
  }
  vkUpdateDescriptorSets(g.device, 9, writes, 0, nullptr);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.prepare);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0,
                          1, &g.set, 0, nullptr);
  // FSR takes reversed depth; NGX and XeSS the device's.
  const auto depthProjection = g.fsr ? reversedDepth(projection) : projection;
  vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(depthProjection), &depthProjection);
  const uint32_t prepareMode = g.fsr ? 2 : 0;
  vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(projection), 4, &prepareMode);
  {
    gpu::GpuScope scope(command, "Upscaler input preparation", g.render);
    vkCmdDispatch(command, (g.render.width + 7) / 8, (g.render.height + 7) / 8,
                  1);
  }
  barrier(command);
  // The upscaler's resolution is its output; its input is the render size.
  gpu::GpuScope upscaleTime(command,
                            g.stats.backend == Backend::DlssRr   ? "DLSS Ray Reconstruction"
                            : g.stats.backend == Backend::DlssSr ? "DLSS Super Resolution"
                            : g.stats.backend == Backend::Xess   ? "XeSS Super Resolution"
                                                                 : "FSR upscaling",
                            g.display);
  const bool resetHistory = reset || !g.initialized;
  try {
    // A failed context uses the already compiled spatial resolve until the
    // runtime activates a prepared replacement. Never retry SDK evaluation.
    if (g.stats.failed)
      throw std::runtime_error(g.stats.failure);
    if (!g.fsr) {
      // NGX and XeSS sample read-only inputs in SHADER_READ_ONLY_OPTIMAL, and
      // leave output in GENERAL. Restore our shared inputs before any later work.
      std::vector<Input> inputs = {color,
                                   {g.depth->image, g.depth->view},
                                   {g.motion->image, g.motion->view}};
      if (g.ngx)
        inputs.push_back({g.exposure->image, g.exposure->view});
      if (g.stats.backend == Backend::DlssRr)
        inputs.insert(inputs.end(),
                      {guides.diffuse, guides.specular, guides.normalRoughness,
                       guides.hitDistance});
      auto transition = [&](bool begin) {
        for (auto input : inputs) {
          VkImageMemoryBarrier b{
              .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
              .srcAccessMask =
                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
              .dstAccessMask =
                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
              .oldLayout = begin ? VK_IMAGE_LAYOUT_GENERAL
                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              .newLayout = begin ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_GENERAL,
              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
              .image = input.image,
              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
          vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                               nullptr, 0, nullptr, 1, &b);
        }
      };
      transition(true);
      try {
        if (g.ngx)
          g.ngx->record(command, color, {g.depth->image, g.depth->view},
                        {g.motion->image, g.motion->view},
                        {g.output->image, g.output->view},
                        {g.exposure->image, g.exposure->view}, guides, view,
                        projection, jitter, milliseconds, resetHistory);
#if REMIX_HAS_XESS
        else if (g.xess) {
          XessDispatch frame;
          frame.color = color;
          frame.depth = input(g.depth);
          frame.motion = input(g.motion);
          frame.output = input(g.output);
          frame.render = g.render;
          // XeSS's jitter shears the projection: the sample moves against it.
          frame.jitter[0] = -jitter[0];
          frame.jitter[1] = -jitter[1];
          frame.reset = resetHistory;
          g.xess->dispatch(command, frame);
        }
#endif
      } catch (...) {
        transition(false);
        throw;
      }
      transition(false);
    } else {
      FsrDispatch frame;
      frame.color = color;
      frame.depth = input(g.depth);
      frame.motion = motion;
      frame.reactive = input(g.reactive);
      frame.composition = input(g.composition);
      frame.dilatedDepth = input(g.dilatedDepth);
      frame.dilatedMotion = input(g.dilatedMotion);
      frame.previousDepth = input(g.previousDepth);
      frame.output = input(g.output);
      frame.render = g.render;
      frame.display = g.display;
      // Rays sample pixel + jitter; FSR subtracts its projected-geometry jitter
      // when locating that sample in the unjittered image.
      frame.jitter[0] = -jitter[0];
      frame.jitter[1] = -jitter[1];
      frame.sharpness = sharpness;
      frame.milliseconds = milliseconds;
      frame.reset = resetHistory;
      const auto range = depthRange(projection);
      // FSR names these endpoints after depth 0/1; inverted depth swaps them.
      frame.cameraNear = range.farPlane;
      frame.cameraFar = range.nearPlane;
      frame.fieldOfView = 2 * std::atan(1 / std::abs(projection[5]));
      g.fsr->dispatch(command, frame);
    }
  } catch (const std::exception &e) {
    if (!g.stats.failed)
      ++g.stats.failures;
    g.stats.failed = true;
    g.stats.failure = e.what();
    barrier(command);
    // Present this frame with a bilinear spatial resolve. Context replacement
    // happens at the next idle frame boundary, never during command recording.
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.prepare);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &g.set, 0, nullptr);
    const uint32_t spatialMode = 1;
    vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(projection), &projection);
    vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, sizeof(projection), 4, &spatialMode);
    vkCmdDispatch(command, (g.display.width + 7) / 8, (g.display.height + 7) / 8, 1);
  }
  barrier(command);
  upscaleTime.close();
  if (g.timestamps)
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        g.timestamps, 1);
  g.timingPending[g.frameIndex] = true;
  ++g.stats.frames;
  if (resetHistory)
    ++g.stats.resets;
  g.initialized = true;
}
void Renderer::configureComposite(VkRenderPass pass, VkImageView stableDepth, VkRenderPass tonemapPass) {
  auto &g = *m_gpu;
  VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
       nullptr}};
  VkDescriptorSetLayoutCreateInfo si{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings};
  check(pipeline::createDescriptorSetLayout(g.device, &si, nullptr,
                                            &g.compositeSetLayout));
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
  VkDescriptorPoolCreateInfo pool{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &size};
  check(vkCreateDescriptorPool(g.device, &pool, nullptr, &g.compositePool));
  VkDescriptorSetAllocateInfo ai{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = g.compositePool,
      .descriptorSetCount = 1,
      .pSetLayouts = &g.compositeSetLayout};
  check(vkAllocateDescriptorSets(g.device, &ai, &g.compositeSet));
  VkDescriptorImageInfo images[] = {
      {VK_NULL_HANDLE, g.output->view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, stableDepth, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[2]{};
  for (unsigned i = 0; i < 2; ++i)
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = g.compositeSet,
                 .dstBinding = i,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .pImageInfo = &images[i]};
  vkUpdateDescriptorSets(g.device, 2, writes, 0, nullptr);
  g.postSetLayout = postfx::compositeLayout(g.device);
  const VkDescriptorSetLayout sets[] = {g.compositeSetLayout, g.postSetLayout};
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
  VkPipelineLayoutCreateInfo li{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 2,
      .pSetLayouts = sets,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push};
  check(pipeline::createPipelineLayout(g.device, &li, nullptr,
                                       &g.compositeLayout));
  VkShaderModule modules[2]{};
  try {
    const uint32_t *codes[] = {vertShader, fragShader};
    const size_t sizes[] = {sizeof(vertShader), sizeof(fragShader)};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (unsigned i = 0; i < 2; ++i) {
      VkShaderModuleCreateInfo mi{
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
          .codeSize = sizes[i],
          .pCode = codes[i]};
      check(pipeline::createShaderModule(g.device, &mi, nullptr, &modules[i]));
      stages[i] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT
                              : VK_SHADER_STAGE_VERTEX_BIT,
                   .module = modules[i],
                   .pName = "main"};
    }
    VkPipelineVertexInputStateCreateInfo vertex{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo samples{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .maxDepthBounds = 1};
    VkPipelineColorBlendAttachmentState attachment{.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &samples,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = g.compositeLayout,
        .renderPass = pass};
    check(pipeline::createGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &ci,
                                            nullptr, &g.compositePipeline));
    if (tonemapPass) {
      ci.renderPass = tonemapPass;
      check(pipeline::createGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &ci,
                                              nullptr, &g.tonemapCompositePipeline));
    }
  } catch (...) {
    for (auto module : modules)
      if (module)
        pipeline::destroyShaderModule(g.device, module, nullptr);
    throw;
  }
  for (auto module : modules)
    pipeline::destroyShaderModule(g.device, module, nullptr);
}
void Renderer::composite(VkCommandBuffer command, const scene::DisplayOutput &display, VkDescriptorSet post) {
  const auto &g = *m_gpu;
  VkViewport viewport{0, 0, float(g.display.width), float(g.display.height),
                      0, 1};
  VkRect2D scissor{{0, 0}, g.display};
  vkCmdSetViewport(command, 0, 1, &viewport);
  vkCmdSetScissor(command, 0, 1, &scissor);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.compositePipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          g.compositeLayout, 0, 1, &g.compositeSet, 0, nullptr);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.compositeLayout, 1, 1, &post, 0, nullptr);
  struct {
    uint32_t display;
    float peakNits, paperWhiteNits;
  } settings{uint32_t(display.hdr10), display.peakNits, display.paperWhiteNits};
  vkCmdPushConstants(command, g.compositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(settings), &settings);
  vkCmdDraw(command, 3, 1, 0, 0);
}

Input Renderer::output() const { return {m_gpu->output->image, m_gpu->output->view}; }
FrameGenerationInputs Renderer::frameGenerationInputs() const {
  const auto &g = *m_gpu;
  if (!g.depth || !g.motion)
    return {};
  // record() writes the device depth for NGX and XeSS, the reversed one for FSR.
  return {{g.depth->image, g.depth->view}, {g.motion->image, g.motion->view}, g.render, bool(g.fsr), true};
}
Input Renderer::preparedDepth() const {
  return m_gpu->depth ? Input{m_gpu->depth->image, m_gpu->depth->view} : Input{};
}
Input Renderer::preparedMotion() const {
  return m_gpu->motion ? Input{m_gpu->motion->image, m_gpu->motion->view} : Input{};
}
} // namespace remix::upscale
