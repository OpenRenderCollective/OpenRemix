#include "forward/lighting.h"
#include "gpu_fixture.h"
#include "shaders/embedded.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace remix;
using namespace remix::forward;
using remix::forward::test::require;
namespace {
  constexpr uint32_t queryCount = 17;
  struct Query {
    Vec4 origin{0, 0, .25f, 1}, incident{0, 0, 1, .005f}, normal{0, 0, -1, 0};
    Vec4 parameters{1, 1, 0, 1}, tint{.5f, .25f, 1, .00001f};
    Matrix projection = scene::identity(), inverseProjection = scene::identity();
  };
  struct Result {
    Vec4 refracted, reflected, through, screen;
  };
  static_assert(sizeof(Query) == 208 && sizeof(Result) == 64);
  Matrix perspective(float sign) {
    // Vulkan zero-to-one depth; near=1, far=10, either camera Z convention.
    Matrix p{};
    p[0] = p[5] = 1;
    p[10] = sign * 10.f / 9;
    p[11] = sign;
    p[14] = -10.f / 9;
    return p;
  }
  struct Queries {
    VkDevice device;
    Buffer input, output, depth;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    explicit Queries(VkDevice target) : device(target) {}
    void initialize(test::Fixture &f) {
      input.create(f.physical, device, sizeof(Query) * queryCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      output.create(f.physical, device, sizeof(Result) * queryCount, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    Buffer::Memory::Readback);
      depth.create(f.physical, device, 16 * 16 * 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      VkDescriptorSetLayoutBinding bindings[3]{};
      for (uint32_t i = 0; i < 3; ++i)
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
      const VkDescriptorSetLayoutCreateInfo setInfo{.sType =
                                                        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                    .bindingCount = 3,
                                                    .pBindings = bindings};
      check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout));
      const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
      const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                  .setLayoutCount = 1,
                                                  .pSetLayouts = &setLayout,
                                                  .pushConstantRangeCount = 1,
                                                  .pPushConstantRanges = &push};
      check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout));
      VkShaderModule module;
      const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                .codeSize = sizeof(test::waterShader),
                                                .pCode = test::waterShader};
      check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module));
      const VkComputePipelineCreateInfo pipelineInfo{
          .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = module,
                    .pName = "main"},
          .layout = layout};
      const auto status =
          vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
      vkDestroyShaderModule(device, module, nullptr);
      check(status);
      const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
      const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                .maxSets = 1,
                                                .poolSizeCount = 1,
                                                .pPoolSizes = &size};
      check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
      const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = pool,
                                                 .descriptorSetCount = 1,
                                                 .pSetLayouts = &setLayout};
      check(vkAllocateDescriptorSets(device, &allocate, &set));
      const VkDescriptorBufferInfo buffers[] = {input.descriptor(), output.descriptor(), depth.descriptor()};
      VkWriteDescriptorSet writes[3]{};
      for (uint32_t i = 0; i < 3; ++i)
        writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set,
                     .dstBinding = i,
                     .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                     .pBufferInfo = &buffers[i]};
      vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
    }
    ~Queries() {
      if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
      if (pipeline)
        vkDestroyPipeline(device, pipeline, nullptr);
      if (layout)
        vkDestroyPipelineLayout(device, layout, nullptr);
      if (setLayout)
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
    std::array<Result, queryCount> run(test::Fixture &f, const std::array<Query, queryCount> &queries, uint32_t samples,
                              bool sky = false, bool foreground = false, float planeDepth = .75f) {
      std::vector<float> depths(16 * 16 * samples, sky ? 1.f : planeDepth);
      if (foreground)
        for (size_t i = samples - 1; i < depths.size(); i += samples)
          depths[i] = .1f;
      input.write(queries.data(), sizeof(queries));
      depth.write(depths.data(), depths.size() * sizeof(float));
      f.begin();
      vkCmdBindPipeline(f.command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(f.command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
      const uint32_t push[] = {queryCount, 16, 16, samples};
      vkCmdPushConstants(f.command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
      vkCmdDispatch(f.command, 1, 1, 1);
      const VkMemoryBarrier read{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(f.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                           &read, 0, nullptr, 0, nullptr);
      f.finish();
      std::array<Result, queryCount> result;
      output.read(result.data(), sizeof(result));
      for (const auto &value : result)
        for (const auto &vector : {value.refracted, value.reflected, value.through, value.screen})
          for (float channel : vector)
            require(std::isfinite(channel), "Water query outputs are finite");
      return result;
    }
  };
} // namespace
int main() try {
  {
    test::Fixture fixture;
    Queries gpu(fixture.device);
    gpu.initialize(fixture);
    if (test::validationErrors != 0)
      throw std::runtime_error("Water query pipeline failed validation");
    std::array<Query, queryCount> queries;
    queries[1].parameters[0] = 1.5f;
    queries[1].incident = {.6f, 0, .8f, .005f};
    queries[2].parameters = {1.5f, 1, 1, 1};
    queries[2].normal = {0, 0, 1, 0};
    queries[3].parameters = {1.5f, 1, 1, 1};
    queries[3].incident = {.9f, 0, std::sqrt(.19f), .005f};
    queries[4].origin[2] = .8f;
    queries[5].origin[0] = .99f;
    queries[5].incident = {1, 0, .1f, .005f};
    queries[6].origin[3] = .2f;
    queries[6].parameters[0] = 1.333f;
    for (uint32_t i : {7u, 8u}) {
      const float sign = i == 7 ? 1.f : -1.f;
      queries[i].origin = {0, 0, sign * 2, 10};
      queries[i].incident = {0, 0, sign, .005f};
      queries[i].normal = {0, 0, -sign, 0};
      queries[i].parameters[3] = sign;
      queries[i].projection = perspective(sign);
      queries[i].inverseProjection = scene::inverse(queries[i].projection);
    }
    for (uint32_t i = 9; i < queryCount; ++i) {
      queries[i].normal[3] = .25f;
      queries[i].parameters = {1.5f, 1, 1, 1};
      queries[i].tint = {1, 1, 1, 0};
    }
    queries[10].tint = {.25f, .5f, 1, 0};
    queries[11].incident = {std::sqrt(.96f), 0, .2f, 0};
    queries[12].incident = {std::sqrt(.75f), 0, .5f, 0};
    queries[12].parameters = {1, 1, 1.5f, 1};
    queries[13].incident = {.6f, 0, .8f, 0};
    queries[14].incident = {.6f, 0, -.8f, 0};
    queries[15].parameters[0] = 1;
    queries[15].incident = {.6f, 0, .8f, 0};
    queries[16].parameters[0] = 1;
    queries[16].normal[3] = 3e38f;
    queries[16].incident = {std::sqrt(.96f), 0, .2f, 0};
    for (uint32_t samples : {1u, 2u, 4u}) {
      const auto values = gpu.run(fixture, queries, samples);
      require(values[0].screen[3] == 1 && std::abs(values[0].screen[2] - .5f) < .0001f,
              "Orthographic refraction recovers the opaque surface distance");
      require(std::abs(values[0].through[0] - std::sqrt(.5f)) < .0001f &&
                  std::abs(values[0].through[1] - .5f) < .0001f && values[0].through[2] == 1,
              "Measured transmission uses the refracted path length with Beer-Lambert absorption");
      require(std::abs(values[1].refracted[0] - .4f) < .00001f &&
                  std::abs(values[1].refracted[2] - std::sqrt(.84f)) < .00001f &&
                  std::abs(values[1].refracted[3] - .0403072f) < .00001f,
              "The primary water interface matches path-traced Snell refraction and Schlick Fresnel");
      require(std::abs(values[2].through[0] - .96f) < .00001f &&
                  values[2].through[0] == values[2].through[1] &&
                  values[2].through[1] == values[2].through[2],
              "An exit boundary does not apply the body's absorption for a second time");
      require(values[3].reflected[3] == 1 && values[3].refracted[3] == 1 && values[3].through[0] == 0 &&
                  values[3].through[3] == 1,
              "Underwater total internal reflection preserves all boundary energy");
      require(std::abs(values[9].reflected[0] - 1.f / 13) < 1e-6f &&
                  std::abs(values[9].through[0] - 12.f / 13) < 1e-6f,
              "A clear index-1.5 sheet includes both interfaces and internal reflections");
      for (uint32_t c = 0; c < 3; ++c) {
        require(std::abs(values[9].reflected[c] + values[9].through[c] - 1) < 1e-6f &&
                    std::abs(values[11].reflected[c] + values[11].through[c] - 1) < 1e-6f &&
                    values[11].reflected[c] > values[9].reflected[c],
                "Lossless sheets conserve energy at normal and grazing incidence");
        const float absorbed = std::pow(queries[10].tint[c], .25f);
        const float expected = .96f * .96f * absorbed / (1 - .04f * .04f * absorbed * absorbed);
        require(std::abs(values[10].through[c] - expected) < 1e-6f &&
                    values[10].through[c] + values[10].reflected[c] <= 1.000001f &&
                    values[12].reflected[c] == 1 && values[12].through[c] == 0,
                "Tinted sheet channels absorb independently and total internal reflection remains lossless");
      }
      const float sideways = .25f * .4f / std::sqrt(.84f);
      require(std::abs(values[13].refracted[0] - sideways) < 1e-6f &&
                  std::abs(values[14].refracted[0] - sideways) < 1e-6f &&
                  std::abs(values[13].refracted[2] - .25f) < 1e-6f &&
                  std::abs(values[14].refracted[2] + .25f) < 1e-6f,
              "Parallel sheet refraction shifts the exit point symmetrically from either side");
      require(std::abs(values[15].refracted[0] - .1875f) < 1e-6f &&
                  std::abs(values[15].refracted[2] - .25f) < 1e-6f &&
                  values[12].refracted[3] == 1 && values[12].refracted[0] == 0,
              "Index-matched sheets preserve a straight path and total internal reflection has no exit");
      std::printf("Extreme sheet: exit %.9g %.9g %.9g, transmission %.9g, reflection %.9g\n",
          values[16].refracted[0], values[16].refracted[1], values[16].refracted[2],
          values[16].through[0], values[16].reflected[0]);
      require(values[16].refracted[0] > 0 && values[16].refracted[2] > 0 &&
                  values[16].through[0] == 1 && values[16].reflected[0] == 0,
              "Extreme clear sheets retain finite exit coordinates and do not invent absorption");
      for (uint32_t i : {4u, 5u, 6u})
        require(values[i].screen[3] == 0,
                "Foreground, off-screen and exhausted rays cannot invent refraction hits");
      require(std::abs(values[6].refracted[3] - .0203732f) < .00001f,
              "Source water's 1.333 IOR retains its path-traced normal-incidence reflectance");
      for (uint32_t i : {7u, 8u})
        require(values[i].screen[3] == 1 && std::abs(values[i].screen[2] - (40.f / 13 - 2)) < .0001f,
                "Perspective depth reconstruction supports both camera Z conventions");
      const auto sky = gpu.run(fixture, queries, samples, true);
      require(sky[0].screen[3] == 0 && sky[0].through[0] == 0,
              "A missing opaque layer remains unknown rather than an arbitrary absorbing depth");
      const auto foreground = gpu.run(fixture, queries, samples, false, true);
      require(foreground[0].screen[3] == 0 && foreground[7].screen[3] == 0,
              "The nearest opaque sample conservatively rejects foreground contamination");
      const auto contact = gpu.run(fixture, queries, samples, false, false, .25f);
      require(contact[0].screen[3] == 1 && contact[0].screen[2] < .0001f && contact[0].through[0] > .9999f,
              "A shoreline contact has zero thickness rather than a missing-layer absorption seam");
      auto longRange = queries;
      longRange[7].origin[3] = longRange[8].origin[3] = 1000;
      const auto overshoot = gpu.run(fixture, longRange, samples);
      for (uint32_t i : {7u, 8u})
        require(overshoot[i].screen[3] == 1 && std::abs(overshoot[i].screen[2] - (40.f / 13 - 2)) < .005f,
                "A coarse step past the far plane still brackets an on-screen opaque hit");
      std::printf("Water boundary/depth queries: %u depth samples, Schlick/Snell/Beer-Lambert, TIR and "
                  "bounded hits passed\n",
                  samples);
    }
  }
  require(test::validationErrors == 0, "Water queries are Vulkan validation clean");
} catch (VkResult result) {
  std::fprintf(stderr, "Water test Vulkan failure %d\n", int(result));
  return 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "Water test failed: %s\n", error.what());
  return 1;
}
