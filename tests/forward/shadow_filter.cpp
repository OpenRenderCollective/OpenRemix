#include "forward/probes/data.h"
#include "gpu_fixture.h"
#include "shadow_shaders/embedded.h"
#include <algorithm>
#include <cmath>
#include <string_view>

using namespace remix;
using namespace remix::forward;
using namespace remix::forward::test;
namespace {
using UInt4 = std::array<uint32_t, 4>;
constexpr uint32_t resolution = 32, sweepSteps = 1001;
struct Query {
  Vec4 position, normal;
  UInt4 control;
};
struct Parameters {
  Vec4 positions[3], settings;
  UInt4 cluster;
  std::array<float, 16> matrices[4];
  Vec4 splits;
  UInt4 target;
};
static_assert(sizeof(Parameters) == 368 && sizeof(Query) == 48);
void upload(Fixture &fixture, Image &image, std::span<const float> pixels, uint32_t layers) {
  Buffer staging;
  staging.create(fixture.physical, fixture.device, pixels.size_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  staging.write(pixels.data(), pixels.size_bytes());
  image.create(fixture.physical, fixture.device, {resolution, resolution}, VK_FORMAT_D32_SFLOAT,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT, layers);
  fixture.begin();
  VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                               .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image.handle,
                               .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers}};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &barrier);
  const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, layers},
                               .imageExtent = {resolution, resolution, 1}};
  vkCmdCopyBufferToImage(fixture.command, staging.handle, image.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &copy);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       0, 0, nullptr, 0, nullptr, 1, &barrier);
  fixture.finish();
}
} // namespace
int main(int argc, char **argv) try {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const bool baseline = argc > 1 && std::string_view(argv[1]) == "--baseline";
  Fixture fixture(false);
  struct Gpu {
    VkDevice device;
    Image fixed, moving, sun;
    Buffer parameters, input, output;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    VkSampler sampler{};
    ~Gpu() {
      if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
      if (pipeline)
        vkDestroyPipeline(device, pipeline, nullptr);
      if (layout)
        vkDestroyPipelineLayout(device, layout, nullptr);
      if (setLayout)
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
      if (sampler)
        vkDestroySampler(device, sampler, nullptr);
    }
  } gpu{fixture.device};
  std::vector<float> fixed(18 * resolution * resolution, 1), moving(fixed.size(), 1),
      sun(8 * resolution * resolution, 1);
  for (uint32_t face = 0; face < 6; ++face)
    for (uint32_t y = 0; y < resolution; ++y)
      for (uint32_t x = 0; x < resolution; ++x) {
        const size_t index = (face * resolution + y) * resolution + x;
        const auto direction = probes::direction(face, 2 * (float(x) + .5f) / resolution - 1,
                                                 2 * (float(y) + .5f) / resolution - 1);
        moving[index] = direction[0] < 0 ? .01f : 1;
        // Slot one is a perfectly planar receiver; every texel has analytic
        // radial depth.
        moving[6 * resolution * resolution + index] =
            direction[2] > 0 ? std::min(1.f, 4 / (direction[2] * 100)) : 1;
        fixed[12 * resolution * resolution + index] = .01f;
      }
  for (uint32_t layer = 0; layer < 8; ++layer)
    for (uint32_t y = 0; y < resolution; ++y)
      for (uint32_t x = 0; x < resolution; ++x)
        sun[(layer * resolution + y) * resolution + x] = layer < 4 ? .25f : x < resolution / 2 ? .1f : 1;
  // A distinct close-up moving layer exposes accidental static reapplication.
  for (uint32_t y = 0; y < resolution; ++y)
    for (uint32_t x = 0; x < resolution; ++x)
      sun[(resolution + y) * resolution + x] = x < resolution / 2 ? .1f : 1;
  upload(fixture, gpu.fixed, fixed, 18);
  upload(fixture, gpu.moving, moving, 18);
  upload(fixture, gpu.sun, sun, 8);
  Parameters parameters{};
  for (auto &position : parameters.positions)
    position = {0, 0, 0, 100};
  parameters.settings = {float(resolution), .01f, .001f, 0};
  parameters.splits = {1, 10, 100, 1000};
  parameters.target[3] = 1;
  for (auto &matrix : parameters.matrices)
    for (uint32_t axis = 0; axis < 4; ++axis)
      matrix[axis * 4 + axis] = 1;
  std::vector<Query> queries;
  for (uint32_t mode = 0; mode < 2; ++mode)
    for (uint32_t i = 0; i < sweepSteps; ++i) {
      const float pixel = 13.5f + 4.f * float(i) / (sweepSteps - 1), u = 2 * (pixel + .5f) / resolution - 1;
      queries.push_back(
          {mode == 0 ? Vec4{u * 4, 0, 4, .5f} : Vec4{u, 0, .5f, .5f}, {0, 0, 1, 0}, {0, 0, mode, 0}});
    }
  const auto planeFirst = queries.size();
  for (float x : {0.f, 3.9f, 4.f, 4.1f, 12.f, 30.f, 60.f})
    for (float y : {0.f, 3.9f, 4.f, 4.1f})
      queries.push_back({{x, y, 4, 0}, {0, 0, 1, 0}, {1, 0, 0, 0}});
  const auto exclusionFirst = queries.size();
  queries.push_back({{0, 0, 4, 0}, {0, 0, 1, 0}, {2, 0, 0, 0}});
  queries.push_back({{0, 0, 4, 0}, {0, 0, 1, 0}, {2, 1, 0, 0}});
  queries.push_back({{0, 0, .5f, .5f}, {0, 0, 1, 0}, {0, 1, 1, 0}});
  const auto viewModelFirst = queries.size();
  queries.push_back({{.75f, 0, .5f, .5f}, {0, 0, 1, 0}, {0, 0, 2, 0}});
  queries.push_back({{.75f, 0, .5f, .5f}, {0, 0, 1, 0}, {0, 1, 2, 0}});
  queries.push_back({{-.75f, 0, .5f, .5f}, {0, 0, 1, 0}, {0, 0, 2, 0}});
  gpu.parameters.create(fixture.physical, fixture.device, sizeof(parameters),
                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  gpu.parameters.write(&parameters, sizeof(parameters));
  gpu.input.create(fixture.physical, fixture.device, queries.size() * sizeof(Query),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.input.write(queries.data(), queries.size() * sizeof(Query));
  gpu.output.create(fixture.physical, fixture.device, queries.size() * sizeof(Vec4),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                    .magFilter = VK_FILTER_NEAREST,
                                    .minFilter = VK_FILTER_NEAREST,
                                    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                    .compareEnable = VK_TRUE,
                                    .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
  check(vkCreateSampler(fixture.device, &sampler, nullptr, &gpu.sampler));
  const VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .bindingCount = uint32_t(std::size(bindings)),
                                                .pBindings = bindings};
  check(vkCreateDescriptorSetLayout(fixture.device, &setInfo, nullptr, &gpu.setLayout));
  const uint32_t count = uint32_t(queries.size());
  const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count)};
  const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                              .setLayoutCount = 1,
                                              .pSetLayouts = &gpu.setLayout,
                                              .pushConstantRangeCount = 1,
                                              .pPushConstantRanges = &range};
  check(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &gpu.layout));
  const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = sizeof(shadow_input::queryShader),
                                            .pCode = shadow_input::queryShader};
  VkShaderModule module;
  check(vkCreateShaderModule(fixture.device, &moduleInfo, nullptr, &module));
  const VkComputePipelineCreateInfo pipelineInfo{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = "main"},
      .layout = gpu.layout};
  const auto pipelineResult =
      vkCreateComputePipelines(fixture.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &gpu.pipeline);
  vkDestroyShaderModule(fixture.device, module, nullptr);
  check(pipelineResult);
  const VkDescriptorPoolSize pools[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}};
  const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = 1,
                                            .poolSizeCount = 3,
                                            .pPoolSizes = pools};
  check(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &gpu.pool));
  const VkDescriptorSetAllocateInfo allocation{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = gpu.pool,
                                               .descriptorSetCount = 1,
                                               .pSetLayouts = &gpu.setLayout};
  VkDescriptorSet set;
  check(vkAllocateDescriptorSets(fixture.device, &allocation, &set));
  const VkDescriptorBufferInfo buffers[] = {gpu.parameters.descriptor(), gpu.input.descriptor(),
                                            gpu.output.descriptor()};
  const VkDescriptorImageInfo images[] = {
      {gpu.sampler, gpu.sun.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {gpu.sampler, gpu.fixed.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {gpu.sampler, gpu.moving.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
  VkWriteDescriptorSet writes[6];
  for (uint32_t i = 0; i < 6; ++i)
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = set,
                 .dstBinding = bindings[i].binding,
                 .descriptorCount = 1,
                 .descriptorType = bindings[i].descriptorType,
                 .pImageInfo = i >= 3 ? &images[i - 3] : nullptr,
                 .pBufferInfo = i < 3 ? &buffers[i] : nullptr};
  vkUpdateDescriptorSets(fixture.device, 6, writes, 0, nullptr);
  fixture.begin();
  vkCmdBindPipeline(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.pipeline);
  vkCmdBindDescriptorSets(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.layout, 0, 1, &set, 0,
                          nullptr);
  vkCmdPushConstants(fixture.command, gpu.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(count), &count);
  vkCmdDispatch(fixture.command, (count + 63) / 64, 1, 1);
  const VkMemoryBarrier read{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                             .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                             .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                       1, &read, 0, nullptr, 0, nullptr);
  fixture.finish();
  std::vector<Vec4> values(queries.size());
  gpu.output.read(values.data(), values.size() * sizeof(Vec4));
  for (uint32_t mode = 0; mode < 2; ++mode) {
    float maximum = 0;
    const size_t first = mode * sweepSteps;
    for (uint32_t i = 0; i < sweepSteps; ++i) {
      const float value = values[first + i][0];
      require(std::isfinite(value) && value >= 0 && value <= 1.00001f,
              "Shadow filter keeps normalized finite visibility");
      if (i) {
        require(value >= values[first + i - 1][0] - 1e-5f, "A straight shadow boundary remains monotonic");
        maximum = std::max(maximum, std::abs(value - values[first + i - 1][0]));
      }
    }
    require(values[first][0] < .001f && values[first + sweepSteps - 1][0] > .999f,
            "Unlit and fully lit endpoints retain their values");
    std::printf("%s shadow: maximum step %.6f across %u subtexel receiver samples\n", mode ? "Sun" : "Point",
                maximum, sweepSteps);
    if (!baseline)
      require(maximum < .01f, "Fractional shadow filtering must not jump at texel boundaries");
  }
  for (size_t i = planeFirst; i < exclusionFirst; ++i)
    require(values[i][0] > .999f, "Grazing planar receivers and cube seams "
                                  "remain free of false self shadowing");
  require(values[exclusionFirst][0] > .999f && values[exclusionFirst + 1][0] < .001f &&
              values[exclusionFirst + 2][0] < .001f,
          "Baked static exclusion and static/dynamic composition remain "
          "independent of filtering");
  require(values[viewModelFirst][0] > .999f && values[viewModelFirst + 1][0] < .001f &&
              values[viewModelFirst + 2][0] < .001f,
          "Viewmodel mixed shadows exclude static occlusion only for baked receivers, retaining moving shadows");
  require(validationErrors == 0, "Shadow filter readback is Vulkan validation clean");
} catch (VkResult error) {
  std::fprintf(stderr, "Shadow filtering GPU test failed: %d\n", int(error));
  return 1;
}
