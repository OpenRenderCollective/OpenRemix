#pragma once
std::vector<std::array<float, 2>> querySunDepth(test::Fixture &fixture, const bake::SunDepthLayout &layout,
    const Buffer &depth, std::span<const bake::Point> receivers,
    const std::array<Vec3, 3> &basis = {Vec3{1,0,0},Vec3{0,1,0},Vec3{0,0,-1}}) {
  struct Query {
    VkDevice device;
    Buffer table, input, output;
    VkDescriptorSetLayout set = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    ~Query() {
      if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
      if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
      if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
      if (set) vkDestroyDescriptorSetLayout(device, set, nullptr);
    }
  } query{fixture.device};
  const auto header = bake::sunDepthHeader(layout, basis, 1, -2, 6, .0001f);
  query.table.create(fixture.physical, fixture.device, sizeof(header) + layout.directory.size() * sizeof(uint32_t),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  query.table.write(&header, sizeof(header));
  if (!layout.directory.empty())
    query.table.write(layout.directory.data(), layout.directory.size() * sizeof(uint32_t), sizeof(header));
  std::vector<Vec4> positions;
  for (const auto &receiver : receivers) positions.push_back(receiver.position);
  query.input.create(fixture.physical, fixture.device, positions.size() * sizeof(Vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  query.input.write(positions.data(), positions.size() * sizeof(Vec4));
  query.output.create(fixture.physical, fixture.device, positions.size() * 2 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  std::array<VkDescriptorSetLayoutBinding, 4> bindings;
  for (uint32_t i = 0; i < bindings.size(); ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = uint32_t(bindings.size()), .pBindings = bindings.data()};
  check(vkCreateDescriptorSetLayout(fixture.device, &setInfo, nullptr, &query.set));
  const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &query.set};
  check(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &query.pipelineLayout));
  const VkShaderModuleCreateInfo shaderInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(test::sun_depth_input::queryShader), .pCode = test::sun_depth_input::queryShader};
  VkShaderModule shader;
  check(vkCreateShaderModule(fixture.device, &shaderInfo, nullptr, &shader));
  const VkComputePipelineCreateInfo pipelineInfo{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = shader, .pName = "main"}, .layout = query.pipelineLayout};
  const auto pipelineResult = vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &query.pipeline);
  vkDestroyShaderModule(fixture.device, shader, nullptr);
  check(pipelineResult);
  const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
  const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
  check(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &query.pool));
  const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = query.pool, .descriptorSetCount = 1, .pSetLayouts = &query.set};
  VkDescriptorSet set;
  check(vkAllocateDescriptorSets(fixture.device, &allocate, &set));
  const VkDescriptorBufferInfo buffers[]{query.table.descriptor(), depth.descriptor(), query.input.descriptor(), query.output.descriptor()};
  std::array<VkWriteDescriptorSet, 4> writes;
  for (uint32_t i = 0; i < writes.size(); ++i)
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i]};
  vkUpdateDescriptorSets(fixture.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
  fixture.begin();
  vkCmdBindPipeline(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, query.pipeline);
  vkCmdBindDescriptorSets(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, query.pipelineLayout, 0, 1, &set, 0, nullptr);
  vkCmdDispatch(fixture.command, (uint32_t(receivers.size()) + 63) / 64, 1, 1);
  const VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
      0, 1, &barrier, 0, nullptr, 0, nullptr);
  fixture.finish();
  std::vector<std::array<float, 2>> result(receivers.size());
  query.output.read(result.data(), result.size() * sizeof(result[0]));
  return result;
}
