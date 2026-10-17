#include "forward/water/snapshot.h"
#include "forward/resources.h"
#include "forward/water/shaders/embedded.h"
#include "platform/gpu_frames.h"
#include "runtime/pipelines.h"
#include <array>
#include <stdexcept>

namespace remix::forward::water {
  struct Snapshot::Gpu {
    VkDevice device = VK_NULL_HANDLE;
    VkExtent2D extent{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    Image hdr, depth;
    VkSampler sampler = VK_NULL_HANDLE;
    VkSampler colorSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, gpuFrameCount> sets{};
    bool ready = false;
    ~Gpu() {
      if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
      if (pipeline)
        pipeline::destroyPipeline(device, pipeline, nullptr);
      if (layout)
        pipeline::destroyPipelineLayout(device, layout, nullptr);
      if (setLayout)
        pipeline::destroyDescriptorSetLayout(device, setLayout, nullptr);
      if (sampler)
        vkDestroySampler(device, sampler, nullptr);
      if (colorSampler)
        vkDestroySampler(device, colorSampler, nullptr);
    }
  };
  Snapshot::Snapshot() = default;
  Snapshot::~Snapshot() = default;
  void Snapshot::initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent,
                            VkSampleCountFlagBits samples) {
    if (!extent.width || !extent.height ||
        (samples != VK_SAMPLE_COUNT_1_BIT && samples != VK_SAMPLE_COUNT_2_BIT &&
         samples != VK_SAMPLE_COUNT_4_BIT))
      throw std::invalid_argument("Invalid water snapshot dimensions or sample count");
    auto next = std::make_unique<Gpu>();
    auto &g = *next;
    g.device = device;
    g.extent = extent;
    g.samples = samples;
    const auto usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    g.hdr.create(physical, device, extent, VK_FORMAT_R16G16B16A16_SFLOAT, usage, VK_SAMPLE_COUNT_1_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT);
    g.depth.create(physical, device, extent, VK_FORMAT_R32_SFLOAT, usage, VK_SAMPLE_COUNT_1_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    const VkSamplerCreateInfo samplerInfo{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                          .magFilter = VK_FILTER_NEAREST,
                                          .minFilter = VK_FILTER_NEAREST,
                                          .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                          .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                          .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    check(vkCreateSampler(device, &samplerInfo, nullptr, &g.sampler));
    VkFormatProperties colorProperties;
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R16G16B16A16_SFLOAT, &colorProperties);
    auto colorSampling = samplerInfo;
    if (colorProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
      colorSampling.magFilter = colorSampling.minFilter = VK_FILTER_LINEAR;
    check(vkCreateSampler(device, &colorSampling, nullptr, &g.colorSampler));
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32_t i = 0; i < 5; ++i)
      bindings[i] = {i, i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo setInfo{.sType =
                                                      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = 5,
                                                  .pBindings = bindings};
    check(pipeline::createDescriptorSetLayout(device, &setInfo, nullptr, &g.setLayout));
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 1,
                                                .pSetLayouts = &g.setLayout,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push};
    check(pipeline::createPipelineLayout(device, &layoutInfo, nullptr, &g.layout));
    const auto module = samples == VK_SAMPLE_COUNT_1_BIT
                            ? shaderModule(device, snapshotSingleShader, sizeof(snapshotSingleShader))
                            : shaderModule(device, snapshotShader, sizeof(snapshotShader));
    const VkComputePipelineCreateInfo compute{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = g.layout};
    const auto result =
        pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr, &g.pipeline);
    pipeline::destroyShaderModule(device, module, nullptr);
    check(result);
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * gpuFrameCount},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * gpuFrameCount}};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = gpuFrameCount,
                                              .poolSizeCount = 2,
                                              .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &g.pool));
    std::array<VkDescriptorSetLayout, gpuFrameCount> layouts;
    layouts.fill(g.setLayout);
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = g.pool,
                                               .descriptorSetCount = gpuFrameCount,
                                               .pSetLayouts = layouts.data()};
    check(vkAllocateDescriptorSets(device, &allocate, g.sets.data()));
    m_gpu = std::move(next);
  }
  void Snapshot::record(VkCommandBuffer command, uint32_t slot, Inputs inputs) {
    if (!m_gpu || !inputs.hdr || !inputs.depth)
      throw std::invalid_argument("Water snapshot requires completed opaque inputs");
    auto &g = *m_gpu;
    for (const auto view : {inputs.hdr, inputs.depth, inputs.reflections.imageView})
      if (view && (view == g.hdr.view || view == g.depth.view))
        throw std::invalid_argument("Water snapshot cannot sample its own output");
    if (inputs.reflections.imageView && inputs.reflections.imageLayout != VK_IMAGE_LAYOUT_GENERAL &&
        inputs.reflections.imageLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
      throw std::invalid_argument("Water snapshot reflections require a readable image layout");
    const auto set = g.sets.at(slot);
    const VkDescriptorImageInfo images[] = {
        {g.sampler, inputs.hdr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {g.sampler, inputs.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {g.sampler, inputs.reflections.imageView ? inputs.reflections.imageView : inputs.hdr,
         inputs.reflections.imageView ? inputs.reflections.imageLayout
                                      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, g.hdr.view, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, g.depth.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t i = 0; i < 5; ++i)
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = set,
                   .dstBinding = i,
                   .descriptorCount = 1,
                   .descriptorType =
                       i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                   .pImageInfo = &images[i]};
    vkUpdateDescriptorSets(g.device, 5, writes, 0, nullptr);
    std::array<VkImageMemoryBarrier, 2> barriers{};
    const VkImage targets[] = {g.hdr.handle, g.depth.handle};
    for (uint32_t i = 0; i < 2; ++i)
      barriers[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                     .srcAccessMask = g.ready ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                                    VK_ACCESS_TRANSFER_READ_BIT
                                              : 0u,
                     .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                     .oldLayout =
                         g.ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                     .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                     .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                     .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                     .image = targets[i],
                     .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    const VkMemoryBarrier inputsReady{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                       VK_ACCESS_SHADER_WRITE_BIT,
                                      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         1, &inputsReady, 0, nullptr, 2, barriers.data());
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &set, 0, nullptr);
    const uint32_t push[] = {g.extent.width, g.extent.height, uint32_t(g.samples),
                             uint32_t(bool(inputs.reflections.imageView))};
    vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(command, (g.extent.width + 7) / 8, (g.extent.height + 7) / 8, 1);
    for (auto &barrier : barriers) {
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 2, barriers.data());
    g.ready = true;
  }
  Snapshot::Output Snapshot::output() const {
    if (!m_gpu)
      throw std::logic_error("Water snapshot is not initialized");
    const auto &g = *m_gpu;
    return {{g.colorSampler, g.hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {g.sampler, g.depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            g.hdr.handle,
            g.depth.handle};
  }
  uint64_t Snapshot::targetBytes() const { return m_gpu ? m_gpu->hdr.bytes + m_gpu->depth.bytes : 0; }
} // namespace remix::forward::water
