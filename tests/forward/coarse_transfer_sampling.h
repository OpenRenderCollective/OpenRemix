#include "shaders/embedded.h"
// Synthetic packed-field queries. CPU reference uses doubles and decodes the
// byte matrix independently; no map or production shader is changed here.
void coarseTransferSampling(test::Fixture &fixture, std::span<const uint8_t> capturedVisibility,
                            std::span<const uint8_t> capturedKnown) {
  struct Query {
    std::array<Vec4,4> weights{};
    std::array<Vec4,16> values{};
    std::array<std::array<uint32_t,4>,4> packed{};
    std::array<uint32_t,4> control{}, known{};
  };
  static_assert(sizeof(Query) == 416);
  std::vector<Query> queries;
  std::vector<Vec4> expected;
  auto append = [&](const Query &q) {
    if (q.control[0] > 8 || q.control[1] > 8) {
      queries.push_back(q); expected.push_back({}); return;
    }
    std::array<double,16> weights{};
    double coarse = 0, fine = 0;
    for (uint32_t i=0; i<16; ++i) weights[i] = q.weights[i/4][i%4];
    for (uint32_t i=0; i<q.control[0]; ++i) coarse += weights[i];
    for (uint32_t i=0; i<q.control[1]; ++i) fine += weights[8+i];
    const double share = coarse+fine > 1e-6 ? fine/(coarse+fine) : 0;
    if (q.control[2] && fine > 1e-6) for (uint32_t c=0; c<q.control[0]; ++c) {
      double visible = 0;
      for (uint32_t f=0; f<q.control[1]; ++f) {
        const auto entry = c*8+f, word = entry/4;
        const bool known = (q.known[entry/32] & (1u << (entry%32))) != 0;
        const auto value = uint8_t(q.packed[word/4][word%4] >> ((entry%4)*8));
        visible += weights[8+f] * (known ? double(value)/255 : 1);
      }
      weights[c] *= 1-share + share*visible/fine;
    }
    double total = 0;
    std::array<double,4> value{};
    for (uint32_t phase=0; phase<2; ++phase)
      for (uint32_t i=0; i<q.control[phase]; ++i) {
        const auto slot = phase*8+i;
        total += weights[slot];
        for (uint32_t c=0; c<4; ++c) value[c] += weights[slot]*q.values[slot][c];
      }
    Vec4 result{};
    if (total >= 1e-4) for (uint32_t c=0; c<4; ++c) result[c] = float(value[c]/total);
    queries.push_back(q); expected.push_back(result);
  };
  for (uint32_t caseIndex=0; caseIndex<256; ++caseIndex) {
    Query q;
    q.control = {1+caseIndex%8, 1+(caseIndex/8)%8, uint32_t(caseIndex%7 != 0), 0};
    const float share = float(caseIndex%17)/16;
    for (uint32_t i=0; i<q.control[0]; ++i) q.weights[i/4][i%4] = (1-share)/q.control[0];
    for (uint32_t i=0; i<q.control[1]; ++i) q.weights[2+i/4][i%4] = share/q.control[1];
    for (uint32_t i=0; i<16; ++i) q.values[i] = {float(i%3)*.1f, float(i)/16, -.2f, 1};
    for (uint32_t entry=0; entry<64; ++entry) {
      const uint32_t value = (entry*37+caseIndex*19)%256, word = entry/4;
      q.packed[word/4][word%4] |= value << ((entry%4)*8);
      if ((entry+caseIndex)%5) q.known[entry/32] |= 1u << (entry%32);
    }
    if (caseIndex%11 == 0) q.known = {}; // Bytes alone cannot establish visibility.
    if (caseIndex%13 == 0 || caseIndex%17 == 0) {
      q.known = {~0u,~0u,0,0};
      for (auto &word : q.packed) word.fill(caseIndex%13 == 0 ? 0u : ~0u);
    }
    append(q);
    // Valid black is distinguishable from absent support through alpha=1.
    for (auto &v : q.values) v = {0,0,0,1};
    append(q);
    q.weights = {}; append(q);
  }
  require(capturedVisibility.size() >= 4 && capturedKnown.size() >= 4,
          "Packed fixture receives the actual hardware-prepared transfer matrix");
  Query captured;
  captured.control = {2,2,1,0};
  captured.weights[0] = {.15f,.05f,0,0};
  captured.weights[2] = {.4f,.4f,0,0};
  captured.values[0] = {1.5f,1.5f,1.5f,1};
  captured.values[1] = {.1f,.1f,.1f,1};
  captured.values[8] = captured.values[9] = {.075f,.075f,.075f,1};
  for (uint32_t c=0; c<2; ++c) for (uint32_t f=0; f<2; ++f) {
    const auto entry = c*8+f, word = entry/4, source = c*2+f;
    captured.packed[word/4][word%4] |= uint32_t(capturedVisibility[source]) << ((entry%4)*8);
    if (capturedKnown[source]) captured.known[entry/32] |= 1u << (entry%32);
  }
  append(captured);
  for (const auto counts : {std::array<uint32_t,2>{8,0}, std::array<uint32_t,2>{0,8},
                            std::array<uint32_t,2>{9,2}, std::array<uint32_t,2>{2,~0u}}) {
    Query q;
    q.control = {counts[0],counts[1],1,0};
    for (auto &w : q.weights) w.fill(.125f);
    for (auto &v : q.values) v = {.5f,.5f,.5f,1};
    append(q);
  }
  struct Gpu {
    VkDevice device;
    Buffer input, output;
    VkDescriptorSetLayout set{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    ~Gpu() {
      if (pool) vkDestroyDescriptorPool(device,pool,nullptr);
      if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
      if (layout) vkDestroyPipelineLayout(device,layout,nullptr);
      if (set) vkDestroyDescriptorSetLayout(device,set,nullptr);
    }
  } gpu{fixture.device};
  gpu.input.create(fixture.physical,fixture.device,queries.size()*sizeof(Query),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.input.write(queries.data(),queries.size()*sizeof(Query));
  gpu.output.create(fixture.physical,fixture.device,expected.size()*sizeof(Vec4),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  const VkDescriptorSetLayoutBinding bindings[] = {
      {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
      {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
  const VkDescriptorSetLayoutCreateInfo setInfo{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount=2,.pBindings=bindings};
  check(vkCreateDescriptorSetLayout(fixture.device,&setInfo,nullptr,&gpu.set));
  const VkPipelineLayoutCreateInfo layoutInfo{.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount=1,.pSetLayouts=&gpu.set};
  check(vkCreatePipelineLayout(fixture.device,&layoutInfo,nullptr,&gpu.layout));
  const VkShaderModuleCreateInfo shaderInfo{.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize=sizeof(test::coarseTransferShader),.pCode=test::coarseTransferShader};
  VkShaderModule shader;
  check(vkCreateShaderModule(fixture.device,&shaderInfo,nullptr,&shader));
  const VkComputePipelineCreateInfo pipelineInfo{.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
              .module=shader,.pName="main"},.layout=gpu.layout};
  const auto result = vkCreateComputePipelines(fixture.device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&gpu.pipeline);
  vkDestroyShaderModule(fixture.device,shader,nullptr); check(result);
  const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2};
  const VkDescriptorPoolCreateInfo poolInfo{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets=1,.poolSizeCount=1,.pPoolSizes=&size};
  check(vkCreateDescriptorPool(fixture.device,&poolInfo,nullptr,&gpu.pool));
  const VkDescriptorSetAllocateInfo allocate{.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool=gpu.pool,.descriptorSetCount=1,.pSetLayouts=&gpu.set};
  VkDescriptorSet set;
  check(vkAllocateDescriptorSets(fixture.device,&allocate,&set));
  const VkDescriptorBufferInfo buffers[]{gpu.input.descriptor(),gpu.output.descriptor()};
  VkWriteDescriptorSet writes[2]{};
  for (uint32_t i=0; i<2; ++i) writes[i] = {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet=set,.dstBinding=i,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo=&buffers[i]};
  vkUpdateDescriptorSets(fixture.device,2,writes,0,nullptr);
  fixture.begin();
  vkCmdBindPipeline(fixture.command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu.pipeline);
  vkCmdBindDescriptorSets(fixture.command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu.layout,0,1,&set,0,nullptr);
  vkCmdDispatch(fixture.command,(uint32_t(queries.size())+63)/64,1,1);
  const VkMemoryBarrier host{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(fixture.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
      0,1,&host,0,nullptr,0,nullptr);
  fixture.finish();
  std::vector<Vec4> values(expected.size());
  gpu.output.read(values.data(),values.size()*sizeof(Vec4));
  for (size_t i=0; i<values.size(); ++i) for (uint32_t c=0; c<4; ++c)
    require(std::isfinite(values[i][c]) && std::abs(values[i][c]-expected[i][c]) < 2e-6f,
            "Packed coarse transfer shader matches independent double-precision reconstruction");
  std::printf("Packed coarse transfer GPU sampling: %zu queries passed\n",queries.size());
}
