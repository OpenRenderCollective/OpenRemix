#include "forward/probes/data.h"
#include "forward/probes/image_asset.h"
#include "forward/probes/priority_order.h"
#include "forward/half.h"
#include "gpu_fixture.h"
#include "probe_shaders/embedded.h"
#include <cmath>
using namespace remix;
using namespace remix::forward;
using namespace remix::forward::test;
// Exercise the image sampler at sizes not yet exposed by the production baker.
// A continuous directional pattern checks face orientation without amplifying
// sub-texel CPU/GPU rounding at an exact face tie into a discontinuous label.
void fillLargeImage(probes::Data &data, uint32_t resolution) {
  const uint32_t levels = std::bit_width(resolution), stride = probes::levelOffset(resolution, levels);
  data.header.parameters[1] = resolution;
  data.header.parameters[2] = levels;
  data.header.parameters[3] = stride;
  data.texels.resize(size_t(data.header.parameters[0]) * stride);
  for (uint32_t probe = 0; probe < data.header.parameters[0]; ++probe)
    for (uint32_t level = 0; level < levels; ++level) {
      const uint32_t size = resolution >> level;
      for (uint32_t face = 0; face < 6; ++face)
        for (uint32_t y = 0; y < size; ++y)
          for (uint32_t x = 0; x < size; ++x) {
            const auto ray = probes::direction(face, 2*(float(x)+.5f)/size-1, 2*(float(y)+.5f)/size-1);
            data.texels[size_t(probe)*stride + probes::levelOffset(resolution, level) + (face*size+y)*size+x] =
                {float(probe + 1) + .5f*ray[0], float(level + 1) + .5f*ray[1], 1 + .5f*ray[2], 0};
          }
    }
}
void samplingCase(uint32_t resolution) {
  Fixture fixture(false);
  struct Gpu {
    VkDevice device;
    Buffer input, output;
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
  std::vector<probes::Capture> captures(3);
  const uint32_t captureSize = std::min(resolution, 128u);
  for (auto &capture : captures) {
    capture.bounds.include({-2, -.5f, -1});
    capture.bounds.include({2, .5f, 1});
    capture.worldToLocal = {{{0, 1, 0, -20}, {-1, 0, 0, 10}, {0, 0, 1, -30}}};
    capture.position = {10, 20.25f, 30};
    capture.resolution = captureSize;
    capture.radiance.resize(6*captureSize*captureSize);
    for (uint32_t face = 0; face < 6; ++face)
      for (uint32_t y = 0; y < captureSize; ++y)
        for (uint32_t x = 0; x < captureSize; ++x) {
          const auto ray = probes::direction(face, 2*(float(x)+.5f)/captureSize-1,
                                            2*(float(y)+.5f)/captureSize-1);
          capture.radiance[(face*captureSize+y)*captureSize+x] = {1+ray[0], 1+ray[1], 1+ray[2], 0};
        }
  }
  // The rotated fallback's influence extends beyond its projection proxy;
  // axis-parallel and diagonal queries must distinguish hits from slab misses.
  captures[0].influence.include({-3, -2, -1});
  captures[0].influence.include({3, 2, 2});
  captures[1].priority = 7;
  captures[1].influence.include({-.25f, -.25f, -1});
  captures[1].influence.include({.25f, .25f, .25f});
  for (auto &pixel : captures[1].radiance) pixel = {4, 0, 0, 0};
  captures[1].blendDistance = .05f;
  const auto original = captures[1];
  constexpr uint32_t axes[] = {2, 0, 1};
  for (uint32_t axis = 0; axis < 3; ++axis) {
    const uint32_t from = axes[axis];
    captures[1].bounds.minimum[axis] = original.bounds.minimum[from];
    captures[1].bounds.maximum[axis] = original.bounds.maximum[from];
    captures[1].influence.minimum[axis] = original.influence.minimum[from];
    captures[1].influence.maximum[axis] = original.influence.maximum[from];
    captures[1].worldToLocal[axis] = original.worldToLocal[from];
  }
  captures[2].position = {10, 19, 30};
  captures[2].priority = 3;
  captures[2].blendDistance = .3f;
  captures[2].influence.include({-2, -.5f, -1});
  captures[2].influence.include({.25f, .5f, 1});
  for (auto &pixel : captures[2].radiance)
    pixel = {0, 0, 3, 0};
  if (resolution == 8) {
    captures.resize(probes::maxProbes);
    for (uint32_t i = 3; i < captures.size(); ++i) {
      captures[i] = captures[0];
      captures[i].position[0] += 1000.f * i;
      captures[i].worldToLocal[1][3] += 1000.f * i;
    }
  }
  auto data = probes::filter(captures, 64);
  if (resolution > 128) fillLargeImage(data, resolution);
  probes::ImageAsset asset(fixture.physical, fixture.device, data);
  auto quantized = data;
  auto references = data;
  const auto normalization = probes::captureNormalization(data);
  const uint32_t stride = data.header.parameters[3];
  for (uint32_t probe = 0; probe < data.header.parameters[0]; ++probe) {
    const auto &row = normalization.coefficients[probe];
    const float value = std::max(0.f, .36f * row[0] + .48f * row[1] + .8f * row[2] + row[3]);
    for (uint32_t texel = 0; texel < stride; ++texel)
      references.texels[size_t(probe) * stride + texel] = {value, value, value, 0};
  }
  for (auto &pixel : quantized.texels)
    for (uint32_t c = 0; c < 3; ++c) pixel[c] = half::decode(half::encode(pixel[c]));
  const uint32_t boundProbe = probes::priorityOrder(data.header).front();
  probes::Data boundData;
  boundData.header = data.header;
  boundData.header.parameters[0] = 1;
  boundData.header.probes[0] = data.header.probes[boundProbe];
  for (uint32_t c = 0; c < 3; ++c) {
    boundData.header.probes[0].influenceMinimum[c] = -1000000;
    boundData.header.probes[0].influenceMaximum[c] = 1000000;
  }
  boundData.texels.assign(quantized.texels.begin() + size_t(boundProbe) * stride,
                          quantized.texels.begin() + size_t(boundProbe + 1) * stride);
  const auto &boundSH = normalization.coefficients[boundProbe];
  const float boundLuma = std::max(0.f, .36f * boundSH[0] + .48f * boundSH[1] + .8f * boundSH[2] + boundSH[3]);
  struct Query { Vec4 position, direction; };
  std::vector<Query> queries;
  const uint32_t steps = 2 * (data.header.parameters[2] - 1);
  for (uint32_t step = 0; step <= steps; ++step)
    for (float x : {9.f, 9.5f, 9.9f, 10.f, 10.2f, 10.5f, 11.f})
      for (float y : {17.f, 18.f, 19.f, 19.5f, 20.f, 20.2f, 20.225f, 20.24999f, 20.25001f, 21.f, 22.f, 23.f})
        for (float z : {29.f, 29.00001f, 29.0001f, 29.001f, 29.01f, 29.05f, 30.f, 31.f})
          for (Vec3 ray : {Vec3{1, 0, 0}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, -1, 0},
                          Vec3{0, 0, 1}, Vec3{0, 0, -1}, Vec3{1, 1, 0}, Vec3{-1, -1, 0},
                          Vec3{1, 0, 1}, Vec3{-1, 0, -1}, Vec3{0, 1, 1}, Vec3{0, -1, -1},
                          Vec3{1, 1, 1}, Vec3{-1, -1, -1}, Vec3{-1, .2f, 1}, Vec3{.1f, -1, 0}, Vec3{1, -2, 0}})
            queries.push_back(
                {{x, y, z, float(step) / steps}, {ray[0], ray[1], ray[2], 0}});
  if (resolution == 8) {
    while (queries.size() % 64) queries.push_back(queries.front());
    for (uint32_t probe = 3; probe < captures.size(); ++probe)
      for (uint32_t lane = 0; lane < 64; ++lane) {
        const auto &p = captures[probe].position;
        queries.push_back({{p[0],p[1],p[2],float(lane)/63}, {1,.2f,.1f,0}});
      }
    queries.push_back(queries.front()); // partial final workgroup participates in every barrier
  }
  const VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 4, .pBindings = bindings};
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
  gpu.input.create(fixture.physical, fixture.device, queries.size()*sizeof(Query), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.input.write(queries.data(), queries.size()*sizeof(Query));
  gpu.output.create(fixture.physical, fixture.device, queries.size()*7*sizeof(Vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  const VkDescriptorPoolSize poolSize[]{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
                                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
  const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = poolSize};
  check(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &gpu.pool));
  const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = gpu.pool, .descriptorSetCount = 1, .pSetLayouts = &gpu.setLayout};
  VkDescriptorSet set;
  check(vkAllocateDescriptorSets(fixture.device, &allocate, &set));
  const VkDescriptorBufferInfo buffers[]{asset.metadata(), gpu.input.descriptor(), gpu.output.descriptor()};
  VkWriteDescriptorSet writes[4];
  for (uint32_t i = 0; i < 3; ++i)
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i]};
  const auto radiance = asset.radiance();
  writes[3] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3,
      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &radiance};
  vkUpdateDescriptorSets(fixture.device, 4, writes, 0, nullptr);
  fixture.begin();
  asset.recordUpload(fixture.command);
  vkCmdBindPipeline(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipeline);
  vkCmdBindDescriptorSets(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.layout, 0, 1, &set, 0, nullptr);
  vkCmdPushConstants(fixture.command, gpu.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count), &count);
  vkCmdDispatch(fixture.command, (count+63)/64, 1, 1);
  const VkMemoryBarrier read{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                       0, 1, &read, 0, nullptr, 0, nullptr);
  fixture.finish();
  asset.releaseUpload();
  std::vector<Vec4> values(count * 7);
  gpu.output.read(values.data(), values.size()*sizeof(Vec4));
  for (size_t i = 0; i < queries.size(); ++i) {
    const auto &q = queries[i];
    const auto expected = probes::sample(quantized, {q.position[0], q.position[1], q.position[2]},
        {q.direction[0], q.direction[1], q.direction[2]}, q.position[3]);
    const auto &actual = values[i * 7];
    require(actual[3] == (expected ? 1.f : 0.f), "GPU oriented influence matches receiver membership");
    for (uint32_t c = 0; c < 3; ++c) {
      const float reference = expected ? (*expected)[c] : 0.f;
      if (std::abs(actual[c] - reference) >= 2e-5f) {
        std::fprintf(stderr, "Reflection mismatch: size %u query %zu channel %u GPU %.9g CPU %.9g; "
            "position %.9g %.9g %.9g roughness %.9g ray %.9g %.9g %.9g\n", resolution, i, c,
            actual[c], reference, q.position[0], q.position[1], q.position[2], q.position[3],
            q.direction[0], q.direction[1], q.direction[2]);
        require(false, "GPU oriented parallax, priorities and roughness match CPU reference");
      }
    }
    if ((i & 63u) == 0u) {
      const auto reference = probes::sample(references, {q.position[0], q.position[1], q.position[2]},
          {q.direction[0], q.direction[1], q.direction[2]}, q.position[3]);
      for (uint32_t mode = 0; mode < 4; ++mode) {
        const float target = mode == 0 ? .25f : mode == 1 ? 0.f : mode == 2 ? 1000000.f : -1.f;
        const float scale = target >= 0 && reference && (*reference)[0] > 0
            ? std::min(target / (*reference)[0], 1.f) : 1.f;
        const auto &normalized = values[i * 7 + 1 + mode];
        require(normalized[3] == (expected ? 1.f : 0.f), "Normalization preserves influence membership");
        for (uint32_t c = 0; c < 3; ++c) {
          const float value = expected ? (*expected)[c] * scale : 0.f;
          require(std::abs(normalized[c] - value) < std::max(3e-5f, std::abs(value) * 5e-5f),
                  "GPU normalization follows blended reference lighting, darkness, gain ceiling and unknown fallback");
        }
      }
      require(values[i * 7 + 5] == Vec4{4, 8, 16, 1}, "A dark capture reference remains neutral");
      const auto bound = probes::sample(boundData, {q.position[0], q.position[1], q.position[2]},
          {q.direction[0], q.direction[1], q.direction[2]}, q.position[3]);
      const auto &assigned = values[i * 7 + 6];
      require(assigned[3] == (bound ? 1.f : 0.f), "Explicit assignments bypass ordinary influence selection");
      for (uint32_t c = 0; c < 3; ++c) {
        const float value = bound ? (*bound)[c] * (boundLuma > 0 ? std::min(.25f / boundLuma, 1.f) : 1.f) : 0;
        require(std::abs(assigned[c] - value) < std::max(3e-5f, std::abs(value) * 5e-5f),
                "Explicit cubemap assignment uses its own matching lighting reference");
      }
    }
  }
  require(validationErrors == 0, "Oriented reflection sampling is Vulkan validation clean");
  std::printf("Oriented reflection GPU sampling at %u: %u translated, rotated, "
              "priority, feather and roughness queries passed\n",
              resolution, count);
}
int main() try {
  for (uint32_t resolution : {8u, 16u, 32u, 64u, 128u, 256u, 512u})
    samplingCase(resolution);
} catch (VkResult error) {
  std::fprintf(stderr, "GPU reflection sampling failed: %d\n", int(error));
  return 1;
}
