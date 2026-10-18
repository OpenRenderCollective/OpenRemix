#include "postfx/renderer.h"
#include "postfx/meter.h"
#include "postfx/shaders/embedded.h"
#include "platform/gpu_timers.h"
#include "runtime/pipelines.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace remix::postfx {
namespace {
void check(VkResult result) {
  if (result != VK_SUCCESS)
    throw result;
}
void barrier(VkCommandBuffer command) {
  VkMemoryBarrier memory{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT};
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0,
                       nullptr, 0, nullptr);
}
struct Buffer {
  VkDevice device{};
  VkBuffer handle{};
  VkDeviceMemory memory{};
  void *mapped{};
  VkDeviceSize bytes{};
  ~Buffer() {
    if (mapped)
      vkUnmapMemory(device, memory);
    if (handle)
      vkDestroyBuffer(device, handle, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
  }
};
struct Image {
  VkDevice device{};
  VkImage handle{};
  VkImageView view{};
  VkDeviceMemory memory{};
  VkExtent2D size{};
  VkDeviceSize bytes{};
  ~Image() {
    if (view)
      vkDestroyImageView(device, view, nullptr);
    if (handle)
      vkDestroyImage(device, handle, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
  }
};
uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits,
                    VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties memory;
  vkGetPhysicalDeviceMemoryProperties(physical, &memory);
  for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
    if ((bits & (1u << i)) &&
        (memory.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  throw VK_ERROR_FEATURE_NOT_PRESENT;
}
std::unique_ptr<Buffer> buffer(VkPhysicalDevice physical, VkDevice device,
                               VkDeviceSize size, VkBufferUsageFlags usage,
                               bool host = false) {
  auto out = std::make_unique<Buffer>();
  out->device = device;
  VkBufferCreateInfo info{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                          .size = size,
                          .usage = usage};
  check(vkCreateBuffer(device, &info, nullptr, &out->handle));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device, out->handle, &req);
  out->bytes = req.size;
  VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = req.size,
                             .memoryTypeIndex = memoryType(
                                 physical, req.memoryTypeBits,
                                 host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  check(vkAllocateMemory(device, &alloc, nullptr, &out->memory));
  check(vkBindBufferMemory(device, out->handle, out->memory, 0));
  if (host)
    check(vkMapMemory(device, out->memory, 0, size, 0, &out->mapped));
  return out;
}
std::unique_ptr<Image> image(VkPhysicalDevice physical, VkDevice device,
                             VkExtent2D size) {
  auto out = std::make_unique<Image>();
  out->device = device;
  out->size = size;
  VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                         .imageType = VK_IMAGE_TYPE_2D,
                         .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                         .extent = {size.width, size.height, 1},
                         .mipLevels = 1,
                         .arrayLayers = 1,
                         .samples = VK_SAMPLE_COUNT_1_BIT,
                         .tiling = VK_IMAGE_TILING_OPTIMAL,
                         .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT};
  check(vkCreateImage(device, &info, nullptr, &out->handle));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, out->handle, &req);
  out->bytes = req.size;
  VkMemoryAllocateInfo alloc{
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = memoryType(physical, req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  check(vkAllocateMemory(device, &alloc, nullptr, &out->memory));
  check(vkBindImageMemory(device, out->handle, out->memory, 0));
  VkImageViewCreateInfo view{
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = out->handle,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = info.format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  check(vkCreateImageView(device, &view, nullptr, &out->view));
  return out;
}
struct Push {
  uint32_t width{}, height{}, groups{};
  float centre{};
  float target{}, blend{}, minEv{}, maxEv{}, radius{};
  uint32_t reset{};
  uint32_t meterMode{};
};
static_assert(sizeof(Push) == 44); // mirrored by shaders/postfx.comp
} // namespace
class State {
public:
  std::unique_ptr<Buffer> exposure;
  bool initialized = false, enabled = false, epochValid = false;
  scene::TemporalEpoch epoch{};
  std::chrono::steady_clock::time_point time{};
};
VkDescriptorSetLayout compositeLayout(VkDevice device) {
  VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
       nullptr}};
  VkDescriptorSetLayoutCreateInfo info{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 3,
      .pBindings = bindings};
  VkDescriptorSetLayout result{};
  check(pipeline::createDescriptorSetLayout(device, &info, nullptr, &result));
  return result;
}
struct Renderer::Gpu {
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkExtent2D display{}, render{};
  std::shared_ptr<State> state;
  bool radianceMeter = false;
  std::unique_ptr<Image> dummy, guide;
  std::vector<std::unique_ptr<Image>> down, up;
  std::unique_ptr<Buffer> partials;
  VkDescriptorSetLayout computeSetLayout{}, compositeSetLayout{};
  VkPipelineLayout layout{};
  VkPipeline pipelines[4]{};
  VkSampler edge{}, border{};
  struct Frame {
    VkDescriptorPool pool{};
    VkDescriptorSet composite{}, meter{};
    std::vector<VkDescriptorSet> down, up;
    std::unique_ptr<Buffer> uniform, readback;
    VkQueryPool timestamps{};
    bool pending = false;
  } frames[gpuFrameCount];
  uint32_t slot = 0;
  bool initialized = false, guideValid = false, bloomValid = false;
  bool activeMeter = false;
  float previousRadius = -1, timestampPeriod = 0;
  std::array<float, 5> previousMeter{};
  unsigned settlingFrames = 0;
  uint64_t sourceGeneration = UINT64_MAX;
  uint32_t timestampBits = 0;
  Statistics stats;
  const char *profile = std::getenv("REMIX_POSTFX_PROFILE");
  ~Gpu() {
    for (auto &frame : frames) {
      if (frame.pool)
        vkDestroyDescriptorPool(device, frame.pool, nullptr);
      if (frame.timestamps)
        vkDestroyQueryPool(device, frame.timestamps, nullptr);
    }
    for (auto p : pipelines)
      if (p)
        pipeline::destroyPipeline(device, p, nullptr);
    if (layout)
      pipeline::destroyPipelineLayout(device, layout, nullptr);
    if (computeSetLayout)
      pipeline::destroyDescriptorSetLayout(device, computeSetLayout, nullptr);
    if (compositeSetLayout)
      pipeline::destroyDescriptorSetLayout(device, compositeSetLayout, nullptr);
    if (edge)
      pipeline::destroySampler(device, edge, nullptr);
    if (border)
      pipeline::destroySampler(device, border, nullptr);
  }
  VkDescriptorSet allocate(Frame &frame, VkDescriptorSetLayout setLayout) {
    VkDescriptorSet result{};
    VkDescriptorSetAllocateInfo info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = frame.pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &setLayout};
    check(vkAllocateDescriptorSets(device, &info, &result));
    return result;
  }
  void descriptors(VkDescriptorSet set, VkImageView source, VkImageView second,
                   VkImageView output, VkSampler sampler,
                   VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_GENERAL,
                   VkImageLayout secondLayout = VK_IMAGE_LAYOUT_GENERAL) {
    VkDescriptorImageInfo images[] = {
        {sampler, source, sourceLayout},
        {edge, second, secondLayout},
        {VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorBufferInfo buffers[] = {{partials->handle, 0, VK_WHOLE_SIZE},
                                        {state->exposure->handle, 0, 16}};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = set,
                   .dstBinding = i,
                   .descriptorCount = 1,
                   .descriptorType =
                       i < 2    ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                       : i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
      if (i < 3)
        writes[i].pImageInfo = &images[i];
      else
        writes[i].pBufferInfo = &buffers[i - 3];
    }
    vkUpdateDescriptorSets(device, 5, writes, 0, nullptr);
  }
  void dispatch(VkCommandBuffer command, uint32_t pass, VkDescriptorSet set,
                const Push &push) {
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pass]);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0,
                            1, &set, 0, nullptr);
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(push), &push);
    unsigned tile = pass == 0 ? 16 : 8;
    vkCmdDispatch(command, pass == 1 ? 1 : (push.width + tile - 1) / tile,
                  pass == 1 ? 1 : (push.height + tile - 1) / tile, 1);
    barrier(command);
  }
};
Renderer::Renderer() = default;
Renderer::~Renderer() = default;
void Renderer::initialize(VkPhysicalDevice physical, VkDevice device,
                          uint32_t family, VkExtent2D display,
                          VkExtent2D render, bool meter, bool bloom,
                          std::shared_ptr<State> state, MeterMode mode) {
  m_gpu = std::make_unique<Gpu>();
  auto &g = *m_gpu;
  g.physical = physical;
  g.device = device;
  g.display = display;
  g.render = render;
  g.radianceMeter = meter && mode == MeterMode::SceneRadiance;
  if (!state) {
    state = std::make_shared<State>();
    state->exposure = buffer(physical, device, 16,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  }
  g.state = std::move(state);
  g.dummy = image(physical, device, {1, 1});
  if (meter && !g.radianceMeter)
    g.guide = image(physical, device, render);
  if (bloom)
    for (auto size : pyramid({display.width, display.height})) {
      g.down.push_back(image(physical, device, {size.width, size.height}));
      if (g.down.size() > 1) {
        auto previous = g.down[g.down.size() - 2]->size;
        g.up.push_back(image(physical, device, previous));
      }
    }
  const auto groups =
      ((display.width + 15) / 16) * ((display.height + 15) / 16);
  g.partials = buffer(physical, device, meter ? groups * 8 : 8,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  VkSamplerCreateInfo sampler{
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
  check(pipeline::createSampler(device, &sampler, nullptr, &g.edge));
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  check(pipeline::createSampler(device, &sampler, nullptr, &g.border));
  g.compositeSetLayout = compositeLayout(device);
  VkDescriptorSetLayoutBinding bindings[5]{};
  for (uint32_t i = 0; i < 5; ++i)
    bindings[i] = {i,
                   i < 2    ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                   : i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo si{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 5,
      .pBindings = bindings};
  check(pipeline::createDescriptorSetLayout(device, &si, nullptr,
                                            &g.computeSetLayout));
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
  VkPipelineLayoutCreateInfo li{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &g.computeSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push};
  check(pipeline::createPipelineLayout(device, &li, nullptr, &g.layout));
  const uint32_t *code[] = {meterShader, resolveShader, downShader, upShader};
  const size_t bytes[] = {sizeof(meterShader), sizeof(resolveShader),
                          sizeof(downShader), sizeof(upShader)};
  for (uint32_t i = 0; i < 4; ++i) {
    if (i < 2 ? !meter : !bloom)
      continue;
    VkShaderModule module{};
    VkShaderModuleCreateInfo mi{.sType =
                                    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                .codeSize = bytes[i],
                                .pCode = code[i]};
    check(pipeline::createShaderModule(device, &mi, nullptr, &module));
    VkComputePipelineCreateInfo pi{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = g.layout};
    auto result = pipeline::createComputePipelines(
        device, VK_NULL_HANDLE, 1, &pi, nullptr, &g.pipelines[i]);
    pipeline::destroyShaderModule(device, module, nullptr);
    check(result);
  }
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(physical, &properties);
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
  g.timestampBits = family == VK_QUEUE_FAMILY_IGNORED ? 0 : families.at(family).timestampValidBits;
  g.timestampPeriod = properties.limits.timestampPeriod;
  uint32_t sets = 2 + g.down.size() + g.up.size();
  for (auto &f : g.frames) {
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pool{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = sets,
        .poolSizeCount = 4,
        .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &pool, nullptr, &f.pool));
    f.composite = g.allocate(f, g.compositeSetLayout);
    f.meter = g.allocate(f, g.computeSetLayout);
    for (size_t i = 0; i < g.down.size(); ++i)
      f.down.push_back(g.allocate(f, g.computeSetLayout));
    for (size_t i = 0; i < g.up.size(); ++i)
      f.up.push_back(g.allocate(f, g.computeSetLayout));
    f.uniform =
        buffer(physical, device, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    f.readback =
        buffer(physical, device, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (g.timestampBits) {
      VkQueryPoolCreateInfo qi{.sType =
                                   VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                               .queryType = VK_QUERY_TYPE_TIMESTAMP,
                               .queryCount = 2};
      check(vkCreateQueryPool(device, &qi, nullptr, &f.timestamps));
    }
    g.stats.memoryBytes += f.uniform->bytes + f.readback->bytes;
  }
  g.stats.memoryBytes +=
      g.state->exposure->bytes + g.partials->bytes + g.dummy->bytes;
  if (g.guide)
    g.stats.memoryBytes += g.guide->bytes;
  for (auto &im : g.down)
    g.stats.memoryBytes += im->bytes;
  for (auto &im : g.up)
    g.stats.memoryBytes += im->bytes;
}
std::shared_ptr<State> Renderer::state() const { return m_gpu->state; }
bool Renderer::supports(VkExtent2D display, VkExtent2D render, bool meter,
                        bool bloom) const {
  const auto &g = *m_gpu;
  return g.display.width == display.width &&
         g.display.height == display.height && g.render.width == render.width &&
         g.render.height == render.height && (!meter || g.guide || g.radianceMeter) &&
         (!bloom || !g.down.empty());
}
void Renderer::selectFrame(uint32_t slot) {
  auto &g = *m_gpu;
  g.slot = slot;
  auto &f = g.frames[slot];
  if (!f.pending)
    return;
  Exposure value;
  std::memcpy(&value, f.readback->mapped, sizeof(value));
  g.stats.exposure = value.current;
  g.stats.target = value.target;
  g.stats.limit = value.limit;
  g.stats.adapting = g.activeMeter && value.initialized == 1 && std::abs(value.current - value.target) >
                                          std::max(value.target, 1e-6f) * .001f;
  if (f.timestamps) {
    uint64_t times[2];
    if (vkGetQueryPoolResults(g.device, f.timestamps, 0, 2, sizeof(times),
                              times, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      const uint64_t mask = UINT64_MAX >> (64 - g.timestampBits);
      g.stats.milliseconds =
          float((times[1] - times[0]) & mask) * g.timestampPeriod / 1e6f;
    }
  }
  if (g.profile && g.activeMeter) {
    if (auto *file = std::fopen(g.profile, "a")) {
      const auto now = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
      std::fprintf(file, "%.6f,%.8g,%.8g,%.0f,%.6f,%llu,%llu,%llu\n", now,
                   g.stats.exposure, g.stats.target, g.stats.limit,
                   g.stats.milliseconds,
                   (unsigned long long)g.stats.memoryBytes,
                   (unsigned long long)g.stats.meterFrames,
                   (unsigned long long)g.stats.bloomFrames);
      std::fclose(file);
    }
  }
  f.pending = false;
}
void Renderer::begin(VkCommandBuffer command) {
  auto &g = *m_gpu;
  if (!g.state->initialized) {
    const Exposure unity;
    vkCmdUpdateBuffer(command, g.state->exposure->handle, 0, sizeof(unity),
                      &unity);
    g.state->initialized = true;
  }
  if (!g.initialized) {
    auto init = [&](Image &im) {
      VkImageMemoryBarrier b{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT |
                           VK_ACCESS_SHADER_READ_BIT |
                           VK_ACCESS_TRANSFER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = im.handle,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
    };
    init(*g.dummy);
    if (g.guide)
      init(*g.guide);
    for (auto &im : g.down)
      init(*im);
    for (auto &im : g.up)
      init(*im);
    const VkClearColorValue black{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(command, g.dummy->handle, VK_IMAGE_LAYOUT_GENERAL,
                         &black, 1, &range);
    g.initialized = true;
  }
  barrier(command);
}
VkImageView Renderer::materialGuide() const {
  return m_gpu->guide ? m_gpu->guide->view : VK_NULL_HANDLE;
}
upscale::Input Renderer::materialInput() const {
  auto &im = m_gpu->guide;
  return im ? upscale::Input{im->handle, im->view} : upscale::Input{};
}
upscale::Input Renderer::bloomOutput() const {
  const auto &g = *m_gpu;
  const auto &im = !g.up.empty()     ? g.up[0]
                   : !g.down.empty() ? g.down[0]
                                     : g.dummy;
  return {im->handle, im->view};
}
bool Renderer::needsGuide() const { return m_gpu->guide && !m_gpu->guideValid; }
void Renderer::freeze() {
  m_gpu->state->time = {};
  m_gpu->stats.adapting = false;
}
void Renderer::record(VkCommandBuffer command, upscale::Input hdr,
                      const TraceOptions &options, scene::TemporalEpoch epoch,
                      bool changed, bool bypass, float seconds,
                      uint64_t generation, MeterInputs inputs) {
  auto &g = *m_gpu;
  auto &f = g.frames[g.slot];
  auto &s = *g.state;
  if (changed) {
    g.guideValid = true;
    g.bloomValid = false;
  }
  const bool hasMeter = g.guide || g.radianceMeter;
  const bool meter = !bypass && options.autoExposure && hasMeter;
  const bool bloom =
      !bypass && options.bloom && !g.down.empty() && options.bloomIntensity > 0;
  g.activeMeter = meter;
  if (!meter)
    g.stats.adapting = false;
  const auto now = std::chrono::steady_clock::now();
  float dt = s.time == std::chrono::steady_clock::time_point{}
                 ? 0.f
                 : std::chrono::duration<float>(now - s.time).count();
  if (seconds >= 0)
    dt = seconds;
  s.time = meter ? now : std::chrono::steady_clock::time_point{};
  const bool reset = !s.enabled || !s.epochValid || s.epoch != epoch;
  // Diagnostic bypass freezes the meter; explicit disabling marks
  // re-enablement.
  if (!bypass && (hasMeter || !options.autoExposure)) {
    s.enabled = options.autoExposure;
    s.epoch = epoch;
    s.epochValid = true;
  }
  if (f.timestamps) {
    vkCmdResetQueryPool(command, f.timestamps, 0, 2);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        f.timestamps, 0);
  }
  barrier(command);
  if (meter) {
    const std::array<float, 5> meterSettings = {
        options.autoExposureTarget, options.autoExposureCentre,
        options.autoExposureMinEv, options.autoExposureMaxEv,
        options.autoExposureSpeed};
    if (meterSettings != g.previousMeter || reset ||
        generation != g.sourceGeneration)
      g.settlingFrames = gpuFrameCount;
    g.previousMeter = meterSettings;
    g.sourceGeneration = generation;
    const auto auxiliary = g.radianceMeter
        ? (inputs.auxiliary.imageView ? inputs.auxiliary.imageView : g.dummy->view) : g.guide->view;
    const auto auxiliaryLayout = g.radianceMeter && inputs.auxiliary.imageView
        ? inputs.auxiliary.imageLayout : VK_IMAGE_LAYOUT_GENERAL;
    g.descriptors(f.meter, hdr.view, auxiliary, g.dummy->view, g.edge,
                  inputs.colorLayout, auxiliaryLayout);
    Push p{g.display.width,
           g.display.height,
           ((g.display.width + 15) / 16) * ((g.display.height + 15) / 16),
           options.autoExposureCentre,
           options.autoExposureTarget,
           adaptation(dt, options.autoExposureSpeed),
           options.autoExposureMinEv,
           options.autoExposureMaxEv,
           0,
           uint32_t(reset),
           uint32_t(g.radianceMeter)};
    {
      gpu::GpuScope scope(command, "Auto exposure meter", g.display);
      g.dispatch(command, 0, f.meter, p);
    }
    {
      gpu::GpuScope scope(command, "Auto exposure resolve", 1, 1);
      g.dispatch(command, 1, f.meter, p);
    }
    ++g.stats.meterFrames;
    if (g.settlingFrames) {
      --g.settlingFrames;
      g.stats.adapting = true;
    }
  }
  if (bloom && (!g.bloomValid || g.previousRadius != options.bloomRadius)) {
    // Sizes: the largest level of each chain.
    gpu::GpuScope downTime(command, "Bloom downsample chain", g.down[0]->size);
    for (size_t i = 0; i < g.down.size(); ++i) {
      auto &im = *g.down[i];
      g.descriptors(f.down[i], i ? g.down[i - 1]->view : hdr.view,
                    g.dummy->view, im.view, g.border,
                    i ? VK_IMAGE_LAYOUT_GENERAL : inputs.colorLayout);
      g.dispatch(command, 2, f.down[i], {im.size.width, im.size.height});
    }
    downTime.close();
    gpu::GpuScope upTime(command, "Bloom upsample chain", g.up.empty() ? VkExtent2D{} : g.up[0]->size);
    for (int i = int(g.up.size()) - 1; i >= 0; --i) {
      auto &im = *g.up[i];
      g.descriptors(f.up[i],
                    i + 1 == int(g.up.size()) ? g.down.back()->view
                                              : g.up[i + 1]->view,
                    g.down[i]->view, im.view, g.edge);
      Push p{im.size.width, im.size.height};
      p.radius = options.bloomRadius;
      g.dispatch(command, 3, f.up[i], p);
    }
    g.previousRadius = options.bloomRadius;
    g.bloomValid = true;
    ++g.stats.bloomFrames;
  }
  const float values[] = {bypass ? 1.f : options.exposure,
                          bloom ? options.bloomIntensity : 0.f,
                          float(!bypass && options.autoExposure), 0};
  std::memcpy(f.uniform->mapped, values, sizeof(values));
  const auto bloomView =
      bloom ? (g.up.empty() ? g.down[0]->view : g.up[0]->view) : g.dummy->view;
  VkDescriptorImageInfo im{g.edge, bloomView, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorBufferInfo buffers[] = {{s.exposure->handle, 0, 16},
                                      {f.uniform->handle, 0, 16}};
  VkWriteDescriptorSet writes[3]{};
  for (uint32_t i = 0; i < 3; ++i) {
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = f.composite,
                 .dstBinding = i,
                 .descriptorCount = 1,
                 .descriptorType =
                     i == 0   ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                     : i == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                              : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER};
    if (i == 1)
      writes[i].pImageInfo = &im;
    else
      writes[i].pBufferInfo = &buffers[i == 0 ? 0 : 1];
  }
  vkUpdateDescriptorSets(g.device, 3, writes, 0, nullptr);
  barrier(command);
  VkBufferCopy copy{0, 0, 16};
  vkCmdCopyBuffer(command, s.exposure->handle, f.readback->handle, 1, &copy);
  barrier(command);
  if (f.timestamps)
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        f.timestamps, 1);
  f.pending = true;
}
VkDescriptorSet Renderer::bindings() const {
  return m_gpu->frames[m_gpu->slot].composite;
}
Statistics Renderer::statistics() const { return m_gpu->stats; }
} // namespace remix::postfx
