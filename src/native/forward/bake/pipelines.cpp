#include "runtime/pipelines.h"
#include "forward/bake/gpu.h"
#include "forward/bake/shaders/embedded.h"
#include <algorithm>

namespace remix::forward::bake {
  VkDescriptorSetLayout Transport::createSceneLayout(VkDevice device) {
    VkDescriptorSetLayoutBinding bindings[10];
    for (uint32_t i = 0; i < 10; ++i) {
      const auto type = i == 0   ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                        : i == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                        : i == 8 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                 : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      const VkShaderStageFlags stages =
          VK_SHADER_STAGE_COMPUTE_BIT | ((i == 6 || i == 7) ? 0u : VK_SHADER_STAGE_FRAGMENT_BIT);
      bindings[i] = {i, type, 1, stages, nullptr};
    }
    const VkDescriptorSetLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = 10,
                                               .pBindings = bindings};
    VkDescriptorSetLayout result;
    check(pipeline::createDescriptorSetLayout(device, &info, nullptr, &result));
    return result;
  }
  void Transport::Gpu::createLayouts() {
    setLayout = Transport::createSceneLayout(device);
    const VkPipelineLayoutCreateInfo info{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                          .setLayoutCount = 1,
                                          .pSetLayouts = &setLayout};
    check(pipeline::createPipelineLayout(device, &info, nullptr, &layout));
    const VkDescriptorSetLayoutBinding atlasBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo atlasSet{.sType =
                                                       VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .bindingCount = 2,
                                                   .pBindings = atlasBindings};
    check(pipeline::createDescriptorSetLayout(device, &atlasSet, nullptr, &atlasSetLayout));
    const VkPipelineLayoutCreateInfo atlasInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .setLayoutCount = 1,
                                               .pSetLayouts = &atlasSetLayout};
    check(pipeline::createPipelineLayout(device, &atlasInfo, nullptr, &atlasLayout));
  }
  void Transport::Gpu::createPipelines() {
    auto compute = [&](const uint32_t *code, size_t bytes, VkPipelineLayout layout) {
      const auto module = shaderModule(device, code, bytes);
      const VkComputePipelineCreateInfo info{
          .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = module,
                    .pName = "main"},
          .layout = layout};
      VkPipeline result;
      const auto status =
          pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result);
      pipeline::destroyShaderModule(device, module, nullptr);
      check(status);
      return result;
    };
    tracePipeline = compute(transportShader, sizeof(transportShader), layout);
    parallelPipeline = compute(parallelShader, sizeof(parallelShader), layout);
    // Keep optional aggregate transport out of the ordinary bake/live kernels.
    // Compile only the variant this immutable policy can actually dispatch.
    if (aggregateDiffuse) {
      if (options.parallel)
        aggregateParallelPipeline = compute(aggregateParallelShader, sizeof(aggregateParallelShader), layout);
      else
        aggregatePipeline = compute(aggregateShader, sizeof(aggregateShader), layout);
    }
    if (options.cpuReadback && options.parallel)
      visibilityPipeline = compute(visibilityShader, sizeof(visibilityShader), layout);
    atlasPipeline = compute(atlasShader, sizeof(atlasShader), atlasLayout);
    const VkSamplerCreateInfo info{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                   .magFilter = VK_FILTER_NEAREST,
                                   .minFilter = VK_FILTER_NEAREST,
                                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    check(pipeline::createSampler(device, &info, nullptr, &sampler));
  }
  void Transport::Gpu::createDescriptors() {
    const uint32_t images = uint32_t(textures.size());
    const uint32_t sets = gpuFrameCount * (1 + uint32_t(FragmentUse::Count));
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, sets},
                                          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, sets},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 7},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets + images},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, std::max(1u, images)}};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = sets + images,
                                              .poolSizeCount = 5,
                                              .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
    const auto acceleration = rays.handle();
    const VkWriteDescriptorSetAccelerationStructureKHR rayInfo{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1,
        .pAccelerationStructures = &acceleration};
    const VkDescriptorImageInfo image{sampler, atlas->view, VK_IMAGE_LAYOUT_GENERAL};
    for (auto &slot : slots) {
      slot.capacity = options.initialBatchPoints ? options.initialBatchPoints : options.batchPoints;
      slot.constants = buffer(nullptr, sizeof(Frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
      slot.points = batchBuffer(size_t(slot.capacity) * sizeof(Point), false);
      slot.results = batchBuffer(size_t(slot.capacity) * sizeof(Result), true);
      const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = pool,
                                                 .descriptorSetCount = 1,
                                                 .pSetLayouts = &setLayout};
      check(vkAllocateDescriptorSets(device, &allocate, &slot.set));
      const VkDescriptorBufferInfo buffers[] = {slot.constants->descriptor(), vertices->descriptor(),
                                                triangles->descriptor(),      materials->descriptor(),
                                                lights->descriptor(),         slot.points->descriptor(),
                                                slot.results->descriptor()};
      VkWriteDescriptorSet writes[10]{};
      writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .pNext = &rayInfo,
                   .dstSet = slot.set,
                   .dstBinding = 0,
                   .descriptorCount = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
      for (uint32_t i = 1; i < 8; ++i)
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = slot.set,
                     .dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType =
                         i == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     .pBufferInfo = &buffers[i - 1]};
      writes[8] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = slot.set,
                   .dstBinding = 8,
                   .descriptorCount = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                   .pImageInfo = &image};
      const auto nodes = lightNodes->descriptor();
      writes[9] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = slot.set,
                   .dstBinding = 9,
                   .descriptorCount = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .pBufferInfo = &nodes};
      vkUpdateDescriptorSets(device, 10, writes, 0, nullptr);
    }
    for (uint32_t i = 0; i < images; ++i) {
      VkDescriptorSet set;
      const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = pool,
                                                 .descriptorSetCount = 1,
                                                 .pSetLayouts = &atlasSetLayout};
      check(vkAllocateDescriptorSets(device, &allocate, &set));
      atlasSets.push_back(set);
    }
  }
} // namespace remix::forward::bake
