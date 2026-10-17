#pragma once
#include "forward/lighting.h"
#include "forward/water/snapshot.h"
#include "gpu_fixture.h"
#include "water_shaders/embedded.h"
#include <vector>

namespace remix::forward::test {
  // Feed real snapshot images directly into the production water boundary and
  // screen-query helpers, rather than reading depth back through the CPU.
  class SnapshotQueries {
  public:
    explicit SnapshotQueries(VkDevice device) : m_device(device) {}
    ~SnapshotQueries() {
      if (m_pool)
        vkDestroyDescriptorPool(m_device, m_pool, nullptr);
      if (m_pipeline)
        vkDestroyPipeline(m_device, m_pipeline, nullptr);
      if (m_layout)
        vkDestroyPipelineLayout(m_device, m_layout, nullptr);
      if (m_setLayout)
        vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
    }
    void initialize(Fixture &fixture, VkExtent2D extent, water::Snapshot::Output snapshot) {
      m_extent = extent;
      m_output.create(fixture.physical, m_device, extent.width * extent.height * sizeof(Vec4),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer::Memory::Readback);
      const VkDescriptorSetLayoutBinding bindings[] = {
          {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
          {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
          {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
      const VkDescriptorSetLayoutCreateInfo setInfo{.sType =
                                                        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                    .bindingCount = 3,
                                                    .pBindings = bindings};
      check(vkCreateDescriptorSetLayout(m_device, &setInfo, nullptr, &m_setLayout));
      const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
      const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                  .setLayoutCount = 1,
                                                  .pSetLayouts = &m_setLayout,
                                                  .pushConstantRangeCount = 1,
                                                  .pPushConstantRanges = &push};
      check(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_layout));
      const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                .codeSize = sizeof(water_input::queryShader),
                                                .pCode = water_input::queryShader};
      VkShaderModule shader;
      check(vkCreateShaderModule(m_device, &moduleInfo, nullptr, &shader));
      const VkComputePipelineCreateInfo info{
          .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = shader,
                    .pName = "main"},
          .layout = m_layout};
      const auto status = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &info, nullptr, &m_pipeline);
      vkDestroyShaderModule(m_device, shader, nullptr);
      check(status);
      const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
      const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                .maxSets = 1,
                                                .poolSizeCount = 2,
                                                .pPoolSizes = sizes};
      check(vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_pool));
      const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = m_pool,
                                                 .descriptorSetCount = 1,
                                                 .pSetLayouts = &m_setLayout};
      check(vkAllocateDescriptorSets(m_device, &allocate, &m_set));
      const VkDescriptorImageInfo images[] = {snapshot.hdr, snapshot.depth};
      const auto buffer = m_output.descriptor();
      const VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                              .dstSet = m_set,
                                              .dstBinding = 0,
                                              .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              .pImageInfo = &images[0]},
                                             {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                              .dstSet = m_set,
                                              .dstBinding = 1,
                                              .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              .pImageInfo = &images[1]},
                                             {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                              .dstSet = m_set,
                                              .dstBinding = 2,
                                              .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .pBufferInfo = &buffer}};
      vkUpdateDescriptorSets(m_device, 3, writes, 0, nullptr);
    }
    void record(VkCommandBuffer command) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0, nullptr);
      const uint32_t push[] = {m_extent.width, m_extent.height};
      vkCmdPushConstants(command, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
      vkCmdDispatch(command, (m_extent.width + 7) / 8, (m_extent.height + 7) / 8, 1);
      const VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                           &host, 0, nullptr, 0, nullptr);
    }
    std::vector<Vec4> read() const {
      std::vector<Vec4> result(m_extent.width * m_extent.height);
      m_output.read(result.data(), result.size() * sizeof(Vec4));
      return result;
    }

  private:
    VkDevice m_device;
    VkExtent2D m_extent{};
    Buffer m_output;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkPipeline m_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
  };
} // namespace remix::forward::test
