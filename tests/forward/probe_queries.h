#pragma once
#include "forward/probes/data.h"
#include "gpu_fixture.h"
#include "shaders/embedded.h"
#include <cmath>

namespace remix::forward::test {
  // Runs the production probe lookup on the GPU against its CPU reference.
  // A host-visible storage buffer supplies the same packed asset that the
  // frame-boundary uploader will copy to device memory.
  inline void probeQueries(Fixture &fixture, const probes::Data &data, uint32_t skyCase = 0) {
    // Sky cases: open, closed, unusable, and hidden behind a distance boundary.
    struct Query {
      Vec4 position, reflected;
    };
    const Query queries[] = {
        {{0, 0, 0, 0}, {0, 0, 1, 0}},   {{.5f, 0, 0, 0}, {0, 0, 1, 0}},   {{0, 0, 0, 0}, {0, 0, 1, 1}},
        {{0, 0, 0, 0}, {1, 0, 1, 0}},   {{2, 0, 0, 0}, {0, 0, 1, 0}},     {{0, 0, 0, 0}, {0, 0, -1, .25f}},
        {{0, 0, 0, 0}, {1, 0, 0, .4f}}, {{1, 1, 1, 0}, {0, -1, 0, .05f}}, {{1.5f, 0, 0, 0}, {1, 0, 0, 0}},
        {{0, 0, 0, 1}, {0, 0, 1, 0}},   {{0, 0, 0, 1}, {0, 0, -1, 0}},    {{4, 0, 0, 1}, {0, 0, 1, 0}},
        {{0, 0, -1, 0}, {0, 0, 1, .2f}}, {{-1, 0, 0, 0}, {1, 0, 0, .4f}},
        {{-.5f, 0, -1, 0}, {0, 0, 1, .3f}}, {{.5f, 0, -1, 0}, {0, 0, 1, .3f}},
        {{0, 0, 0, 2}, {0, 0, 1, 0}}, {{.5f, 0, 0, 2}, {1, 0, 0, 0}},
        {{-.5f, .2f, 0, 2}, {0, 1, 0, 0}}, {{4, 0, 0, 2}, {0, 0, 1, 0}}};
    struct Gpu {
      VkDevice device;
      Buffer asset, input, output, sky;
      VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
      VkPipelineLayout layout = VK_NULL_HANDLE;
      VkPipeline pipeline = VK_NULL_HANDLE;
      VkDescriptorPool pool = VK_NULL_HANDLE;
      ~Gpu() {
        if (pool)
          vkDestroyDescriptorPool(device, pool, nullptr);
        if (pipeline)
          vkDestroyPipeline(device, pipeline, nullptr);
        if (layout)
          vkDestroyPipelineLayout(device, layout, nullptr);
        if (setLayout)
          vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
      }
    } gpu{fixture.device};
    const auto packed = probes::serialize(data);
    gpu.asset.create(fixture.physical, fixture.device, packed.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    gpu.asset.write(packed.data(), packed.size());
    struct SkyGrid {
      bake::GridHeader header;
      bake::Result probes[8];
    } sky{};
    sky.header.originSpacing = {-1, -1, -1, 2};
    sky.header.dimensions = {2, 2, 2, 8};
    sky.header.settings = {0, .0001f, 0, 0};
    for (uint32_t i = 0; i < 8; ++i) {
      auto &probe = sky.probes[i];
      probe.statistics[3] = skyCase == 2 ? 0.f : 1.f;
      probe.irradiance[0] = {.2f + i * .1f, .5f, .8f - i * .05f, 0};
      probe.irradiance[1] = {-.05f, .1f, .05f, 0};
      if (skyCase == 4) {
        sky.header.settings[2] = 1;
        probe.irradiance[0][3] = i & 1 ? -.7f : .7f;
        probe.irradiance[1][3] = i & 2 ? -.4f : .4f;
        probe.irradiance[2][3] = i & 4 ? -.2f : .2f;
      }
      for (auto &moment : probe.moments)
        moment = skyCase == 3 ? Vec4{0, 0, 0, 16} : Vec4{100, 10000, skyCase == 1 ? 1.f : 0.f, 16};
    }
    gpu.sky.create(fixture.physical, fixture.device, sizeof(sky), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    gpu.sky.write(&sky, sizeof(sky));
    bake::Grid reference;
    reference.header = sky.header;
    reference.probes.assign(std::begin(sky.probes), std::end(sky.probes));
    gpu.input.create(fixture.physical, fixture.device, sizeof(queries), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    gpu.input.write(queries, sizeof(queries));
    gpu.output.create(fixture.physical, fixture.device, std::size(queries) * sizeof(Vec4),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo setInfo{.sType =
                                                      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = 4,
                                                  .pBindings = bindings};
    check(vkCreateDescriptorSetLayout(fixture.device, &setInfo, nullptr, &gpu.setLayout));
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 1,
                                                .pSetLayouts = &gpu.setLayout,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &push};
    check(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &gpu.layout));
    VkShaderModule module;
    const VkShaderModuleCreateInfo shaderInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                              .codeSize = sizeof(probesShader),
                                              .pCode = probesShader};
    check(vkCreateShaderModule(fixture.device, &shaderInfo, nullptr, &module));
    const VkComputePipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module,
                  .pName = "main"},
        .layout = gpu.layout};
    const auto result =
        vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu.pipeline);
    vkDestroyShaderModule(fixture.device, module, nullptr);
    check(result);
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = 1,
                                              .poolSizeCount = 1,
                                              .pPoolSizes = &size};
    check(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &gpu.pool));
    VkDescriptorSet set;
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = gpu.pool,
                                               .descriptorSetCount = 1,
                                               .pSetLayouts = &gpu.setLayout};
    check(vkAllocateDescriptorSets(fixture.device, &allocate, &set));
    const VkDescriptorBufferInfo buffers[] = {gpu.input.descriptor(), gpu.output.descriptor(),
                                              gpu.asset.descriptor(), gpu.sky.descriptor()};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i)
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = set,
                   .dstBinding = bindings[i].binding,
                   .descriptorCount = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .pBufferInfo = &buffers[i]};
    vkUpdateDescriptorSets(fixture.device, 4, writes, 0, nullptr);
    fixture.begin();
    vkCmdBindPipeline(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipeline);
    vkCmdBindDescriptorSets(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.layout, 0, 1, &set, 0,
                            nullptr);
    const uint32_t count = uint32_t(std::size(queries));
    vkCmdPushConstants(fixture.command, gpu.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &count);
    vkCmdDispatch(fixture.command, 1, 1, 1);
    const VkMemoryBarrier read{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &read, 0, nullptr, 0, nullptr);
    fixture.finish();
    Vec4 values[std::size(queries)];
    gpu.output.read(values, sizeof(values));
    for (uint32_t i = 0; i < count; ++i) {
      const auto &q = queries[i];
      if (q.position[3] == 2) {
        const auto expected = bake::sample(reference, {q.position[0], q.position[1], q.position[2]},
                                           {q.reflected[0], q.reflected[1], q.reflected[2]});
        require((values[i][3] > .5f) == bool(expected), "GPU relocated diffuse coverage matches CPU sampling");
        if (expected)
          for (uint32_t c = 0; c < 3; ++c)
            require(std::abs(values[i][c] - (*expected)[c]) < 2e-5f,
                    "GPU diffuse interpolation uses actual relocated trace origins");
        continue;
      }
      if (q.position[3] > 0) {
        const float expected = (skyCase == 0 || skyCase == 4) && q.position[0] == 0 ? 1.f : 0.f;
        require(std::abs(values[i][0] - expected) < 1e-5f,
                "Sky visibility accepts open space and rejects closed, invalid, occluded and unknown space");
        continue;
      }
      const auto expected = probes::sample(data, {q.position[0], q.position[1], q.position[2]},
                                           {q.reflected[0], q.reflected[1], q.reflected[2]}, q.reflected[3]);
      require((values[i][3] > .5f) == bool(expected), "GPU probe influence matches the CPU reference");
      if (expected)
        for (uint32_t c = 0; c < 3; ++c)
          require(std::abs(values[i][c] - (*expected)[c]) < 2e-4f,
                  "GPU cube projection/filter matches the CPU reference");
    }
  }
} // namespace remix::forward::test
