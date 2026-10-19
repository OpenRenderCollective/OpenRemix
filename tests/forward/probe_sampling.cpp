#include "forward/probes/data.h"
#include "gpu_fixture.h"
#include "probe_shaders/embedded.h"
#include <cmath>
using namespace remix;
using namespace remix::forward;
using namespace remix::forward::test;
int main() try {
  Fixture fixture(false);
  struct Gpu {
    VkDevice device;
    Buffer asset, input, output;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    ~Gpu() {
      if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
      if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
      if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
      if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
  } gpu{fixture.device};
  probes::Capture captures[2];
  for (auto &capture : captures) {
    capture.bounds.include({-2, -.5f, -1});
    capture.bounds.include({2, .5f, 1});
    capture.worldToLocal = {{{0, 1, 0, -20}, {-1, 0, 0, 10}, {0, 0, 1, -30}}};
    capture.position = {10, 20.25f, 30};
    capture.resolution = 8;
    capture.radiance.resize(6*8*8);
    for (uint32_t face = 0; face < 6; ++face)
      for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
          const auto ray = probes::direction(face, 2*(float(x)+.5f)/8-1, 2*(float(y)+.5f)/8-1);
          capture.radiance[(face*8+y)*8+x] = {1+ray[0], 1+ray[1], 1+ray[2], 0};
        }
  }
  captures[1].priority = 7;
  captures[1].influence.include({-.25f, -.25f, -.25f});
  captures[1].influence.include({.25f, .25f, .25f});
  for (auto &pixel : captures[1].radiance) pixel = {4, 0, 0, 0};
  const auto data = probes::filter(captures, 64);
  const auto bytes = probes::serialize(data);
  struct Query { Vec4 position, direction; };
  std::vector<Query> queries;
  for (float roughness : {0.f, .3f, 1.f})
    for (float x : {9.f, 9.5f, 9.9f, 10.f, 10.2f, 10.5f, 11.f})
      for (float y : {17.f, 18.f, 19.f, 20.f, 20.2f, 21.f, 22.f, 23.f})
        for (Vec3 ray : {Vec3{1, 0, 0}, Vec3{0, 1, 1}, Vec3{-1, .2f, 1}})
          queries.push_back({{x, y, 30, roughness}, {ray[0], ray[1], ray[2], 0}});
  const VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 3, .pBindings = bindings};
  check(vkCreateDescriptorSetLayout(fixture.device, &setInfo, nullptr, &gpu.setLayout));
  const uint32_t count = uint32_t(queries.size());
  const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count)};
  const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &gpu.setLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
  check(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &gpu.layout));
  VkShaderModule module;
  const VkShaderModuleCreateInfo shaderInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(probe_input::reflectionShader), .pCode = probe_input::reflectionShader};
  check(vkCreateShaderModule(fixture.device, &shaderInfo, nullptr, &module));
  const VkComputePipelineCreateInfo pipelineInfo{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module, .pName = "main"}, .layout = gpu.layout};
  const auto result = vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu.pipeline);
  vkDestroyShaderModule(fixture.device, module, nullptr);
  check(result);
  gpu.asset.create(fixture.physical, fixture.device, bytes.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.asset.write(bytes.data(), bytes.size());
  gpu.input.create(fixture.physical, fixture.device, queries.size()*sizeof(Query), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.input.write(queries.data(), queries.size()*sizeof(Query));
  gpu.output.create(fixture.physical, fixture.device, queries.size()*sizeof(Vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
  const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &poolSize};
  check(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &gpu.pool));
  const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = gpu.pool, .descriptorSetCount = 1, .pSetLayouts = &gpu.setLayout};
  VkDescriptorSet set;
  check(vkAllocateDescriptorSets(fixture.device, &allocate, &set));
  const VkDescriptorBufferInfo buffers[]{gpu.asset.descriptor(), gpu.input.descriptor(), gpu.output.descriptor()};
  VkWriteDescriptorSet writes[3];
  for (uint32_t i = 0; i < 3; ++i)
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i]};
  vkUpdateDescriptorSets(fixture.device, 3, writes, 0, nullptr);
  fixture.begin();
  vkCmdBindPipeline(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipeline);
  vkCmdBindDescriptorSets(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.layout, 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(fixture.command, gpu.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count), &count);
  vkCmdDispatch(fixture.command, (count+63)/64, 1, 1);
  const VkMemoryBarrier read{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       0, 1, &read, 0, nullptr, 0, nullptr);
  fixture.finish();
  std::vector<Vec4> values(count);
  gpu.output.read(values.data(), values.size()*sizeof(Vec4));
  for (size_t i = 0; i < queries.size(); ++i) {
    const auto &q = queries[i];
    const auto expected = probes::sample(data, {q.position[0], q.position[1], q.position[2]},
        {q.direction[0], q.direction[1], q.direction[2]}, q.position[3]);
    require(values[i][3] == (expected ? 1.f : 0.f), "GPU oriented influence matches receiver membership");
    for (uint32_t c = 0; c < 3; ++c)
      require(std::abs(values[i][c] - (expected ? (*expected)[c] : 0.f)) < 2e-5f,
              "GPU oriented parallax, priorities and roughness match CPU reference");
  }
  require(validationErrors == 0, "Oriented reflection sampling is Vulkan validation clean");
  std::printf("Oriented reflection GPU sampling: %u translated, rotated, priority and roughness queries passed\n", count);
} catch (VkResult error) {
  std::fprintf(stderr, "GPU reflection sampling failed: %d\n", int(error));
  return 1;
}
