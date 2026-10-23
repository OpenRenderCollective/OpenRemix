#pragma once
#include "forward/bake/controller.h"
#include "forward/bake/probe_visibility.h"
#include "forward/probe_batch/batch.h"
#include "gpu_fixture.h"
#include "probe_batch_shaders/embedded.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace remix::forward::test {
  // GPU reference comparison for the receiver-side vector reconstruction,
  // independent of the producer's already bit-exact readback checks.
  inline void queryField(Fixture &fixture, const bake::Volume &volume,
                         const bake::ProbeVisibility &masks, const probe_batch::Field &field) {
    struct Query {
      std::array<float, 16> weights{};
      std::array<uint32_t, 16> indices{};
      std::array<uint32_t, 4> control{8, 1, 0, 0};
    };
    static_assert(sizeof(Query) == 144);
    std::array<Query, 9> queries;
    for (auto &query : queries)
      for (uint32_t i = 0; i < 16; ++i) {
        query.weights[i] = float(i + 1) / 16;
        query.indices[i] = i % 8;
      }
    queries[1].weights[0] = 5e-7f;
    queries[2].weights[0] = 1e-6f;
    queries[3].weights[0] = 1.1e-6f;
    queries[4].weights.fill(0);
    queries[5].weights.fill(1e-7f);
    queries[6].control[1] = 0;
    queries[7].control[0] = 16;
    queries[8].weights[0] = -1;
    queries[8].weights[3] = 0;
    struct Resources {
      VkDevice device;
      VkDescriptorSetLayout set{};
      VkPipelineLayout layout{};
      VkPipeline pipeline{};
      VkDescriptorPool pool{};
      Buffer input, output, host;
      ~Resources() {
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (set) vkDestroyDescriptorSetLayout(device, set, nullptr);
      }
    } gpu{fixture.device};
    std::vector<std::byte> input(64 * sizeof(uint32_t) + sizeof(queries));
    std::memcpy(input.data(), field.lights().data(), field.lights().size_bytes());
    std::memcpy(input.data() + 64 * sizeof(uint32_t), queries.data(), sizeof(queries));
    gpu.input.create(fixture.physical, fixture.device, input.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    gpu.input.write(input.data(), input.size());
    const size_t values = queries.size() * 2 * (field.lights().size() + 1);
    gpu.output.create(fixture.physical, fixture.device, values * sizeof(Vec4),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Memory::Device);
    gpu.host.create(fixture.physical, fixture.device, values * sizeof(Vec4),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
    VkDescriptorSetLayoutBinding bindings[4];
    const uint32_t slots[]{0, 1, 2, 22};
    for (uint32_t i = 0; i < 4; ++i)
      bindings[i] = {slots[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4, .pBindings = bindings};
    check(vkCreateDescriptorSetLayout(fixture.device, &setInfo, nullptr, &gpu.set));
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &gpu.set, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    check(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &gpu.layout));
    const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(probe_batch_input::queryShader), .pCode = probe_batch_input::queryShader};
    VkShaderModule module;
    check(vkCreateShaderModule(fixture.device, &moduleInfo, nullptr, &module));
    const uint32_t enabled = 1;
    const VkSpecializationMapEntry entry{5, 0, sizeof(enabled)};
    const VkSpecializationInfo specialization{1, &entry, sizeof(enabled), &enabled};
    const VkComputePipelineCreateInfo pipelineInfo{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main",
            .pSpecializationInfo = &specialization}, .layout = gpu.layout};
    const auto created = vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu.pipeline);
    vkDestroyShaderModule(fixture.device, module, nullptr);
    check(created);
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    check(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &gpu.pool));
    const VkDescriptorSetAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = gpu.pool, .descriptorSetCount = 1, .pSetLayouts = &gpu.set};
    VkDescriptorSet set;
    check(vkAllocateDescriptorSets(fixture.device, &allocation, &set));
    const VkDescriptorBufferInfo buffers[]{volume.data->descriptor(), gpu.input.descriptor(),
        gpu.output.descriptor(), field.descriptor()};
    VkWriteDescriptorSet writes[4];
    for (uint32_t i = 0; i < 4; ++i)
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = slots[i],
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(fixture.device, 4, writes, 0, nullptr);
    fixture.begin();
    const VkMemoryBarrier ready{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &ready, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipeline);
    vkCmdBindDescriptorSets(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.layout, 0, 1, &set, 0, nullptr);
    const uint32_t count = uint32_t(queries.size());
    vkCmdPushConstants(fixture.command, gpu.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count), &count);
    vkCmdDispatch(fixture.command, 1, 1, 1);
    const VkMemoryBarrier copied{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &copied, 0, nullptr, 0, nullptr);
    const VkBufferCopy copy{0, 0, gpu.output.capacity};
    vkCmdCopyBuffer(fixture.command, gpu.output.handle, gpu.host.handle, 1, &copy);
    const VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    fixture.finish();
    std::vector<Vec4> results(values);
    gpu.host.read(results.data(), results.size() * sizeof(Vec4));
    for (uint32_t query = 0; query < queries.size(); ++query)
      for (uint32_t phase = 0; phase < 2; ++phase)
        for (uint32_t iteration = 0; iteration <= field.lights().size(); ++iteration) {
          const auto &q = queries[query];
          const auto n = uint32_t(field.lights().size());
          const uint32_t slot = iteration == n ? 0 : iteration % 2 == 0 ? iteration / 2 : n - 1 - iteration / 2;
          const auto captured = field.lights()[slot];
          float sum = 0, total = 0;
          bool complete = q.control[1] != 0;
          for (uint32_t i = 0; i < q.control[0]; ++i) {
            const float weight = q.weights[phase == 0 ? i : q.control[0] - 1 - i];
            if (weight <= 0) continue;
            const auto mask = captured ? masks.sample(q.indices[i], captured - 1) : std::optional<float>{};
            if (!mask) { if (weight > 1e-6f) complete = false; continue; }
            sum += weight * *mask;
            total += weight;
          }
          complete = complete && total >= 1e-4f;
          const auto &value = results[(query * 2 + phase) * (n + 1) + iteration];
          require((value[2] != 0) == complete && (value[3] != 0) == complete,
              "Batched and sparse reconstruction preserve weighted unknown coverage and unsupported stencils");
          if (complete)
            require(std::abs(value[0] - sum / total) < 2e-6f && std::abs(value[1] - sum / total) < 2e-6f,
                "Vector groups retain scalar normalization through group switches and stencil revisions");
        }
  }
}
