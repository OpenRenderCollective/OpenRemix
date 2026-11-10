#include "gpu_fixture.h"
#include "shaders/embedded.h"
#include <cmath>
#include <stdexcept>

using namespace remix::forward;
using namespace remix::forward::test;

namespace {
  struct Ray {
    std::array<float, 4> origin, direction;
    std::array<uint32_t, 4> selection{UINT32_MAX, 0, 0, 0};
  };
  static_assert(sizeof(Ray) == 48);
  const Ray rays[] = {
      {{0, 0, 0, .001f}, {0, 0, 1, 10}},               // closest
      {{3, 0, 0, .001f}, {0, 0, 1, 10}},               // outside
      {{0, 0, 0, .001f}, {0, 0, 1, 10}, {0, 0, 0, 0}}, // reject front candidate, keep rear
      {{0, 0, 0, .001f}, {0, 0, 1, .5f}},              // short ray
      {{0, 0, 3, .001f}, {0, 0, -1, 10}},              // back-face query
      {{0, 0, 0, 1.5f}, {0, 0, 1, 10}},                // minimum distance
  };
  struct Queries {
    Fixture &fixture;
    Buffer input, output;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    explicit Queries(Fixture &fixture) : fixture(fixture) {
      const auto device = fixture.device;
      input.create(fixture.physical, device, sizeof(rays), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      input.write(rays, sizeof(rays));
      output.create(fixture.physical, device, std::size(rays) * sizeof(std::array<float, 4>),
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      const VkDescriptorSetLayoutBinding bindings[] = {
          {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
          {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
      const VkDescriptorSetLayoutCreateInfo setInfo{.sType =
                                                        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                    .bindingCount = 3,
                                                    .pBindings = bindings};
      check(vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout));
      const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
      const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                  .setLayoutCount = 1,
                                                  .pSetLayouts = &setLayout,
                                                  .pushConstantRangeCount = 1,
                                                  .pPushConstantRanges = &push};
      check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout));
      VkShaderModule module;
      const VkShaderModuleCreateInfo shader{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = sizeof(compShader),
                                            .pCode = compShader};
      check(vkCreateShaderModule(device, &shader, nullptr, &module));
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
      const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
      const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                                .maxSets = 1,
                                                .poolSizeCount = 2,
                                                .pPoolSizes = sizes};
      check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
      const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                 .descriptorPool = pool,
                                                 .descriptorSetCount = 1,
                                                 .pSetLayouts = &setLayout};
      check(vkAllocateDescriptorSets(device, &allocate, &set));
      const auto inputInfo = input.descriptor(), outputInfo = output.descriptor();
      const VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                              .dstSet = set,
                                              .dstBinding = 1,
                                              .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .pBufferInfo = &inputInfo},
                                             {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                              .dstSet = set,
                                              .dstBinding = 2,
                                              .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .pBufferInfo = &outputInfo}};
      vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    }
    ~Queries() {
      vkDestroyDescriptorPool(fixture.device, pool, nullptr);
      vkDestroyPipeline(fixture.device, pipeline, nullptr);
      vkDestroyPipelineLayout(fixture.device, layout, nullptr);
      vkDestroyDescriptorSetLayout(fixture.device, setLayout, nullptr);
    }
    void scene(const RayScene &scene) {
      const auto handle = scene.handle();
      const VkWriteDescriptorSetAccelerationStructureKHR acceleration{
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
          .accelerationStructureCount = 1,
          .pAccelerationStructures = &handle};
      const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .pNext = &acceleration,
                                       .dstSet = set,
                                       .dstBinding = 0,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
      vkUpdateDescriptorSets(fixture.device, 1, &write, 0, nullptr);
    }
    void record() {
      const auto command = fixture.command;
      const VkBufferMemoryBarrier reuse{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .buffer = output.handle,
                                        .offset = 0,
                                        .size = VK_WHOLE_SIZE};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &reuse, 0, nullptr);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
      const uint32_t count = uint32_t(std::size(rays));
      vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &count);
      vkCmdDispatch(command, 1, 1, 1);
      const VkMemoryBarrier read{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                           &read, 0, nullptr, 0, nullptr);
    }
    void verify(bool empty = false) const {
      std::array<std::array<float, 4>, std::size(rays)> hits;
      output.read(hits.data(), sizeof(hits));
      const float distances[] = {1, -1, 2, -1, 1, 2};
      const float primitives[] = {0, -1, 1, -1, 1, 1};
      for (size_t i = 0; i < hits.size(); ++i) {
        require(std::abs(hits[i][0] - (empty ? -1 : distances[i])) < 1e-5f, "Hardware query hit distance");
        require(hits[i][1] == (empty ? -1 : primitives[i]),
                "Hardware query primitive order / candidate rejection");
        if (hits[i][0] >= 0)
          require(hits[i][2] >= 0 && hits[i][3] >= 0 && hits[i][2] + hits[i][3] <= 1,
                  "Hardware query barycentrics");
      }
    }
  };
} // namespace
int main() try {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  {
    Fixture fixture;
    {
      RayScene scene;
      RayScene::Position positions[] = {{-1, -1, 1}, {1, -1, 1}, {0, 1, 1},
                                        {-1, -1, 2}, {1, -1, 2}, {0, 1, 2}};
      const uint32_t indices[] = {0, 1, 2, 3, 4, 5};
      scene.initialize(fixture.physical, fixture.device, {positions, indices});
      positions[0][0] = 100; // Caller's data is no longer the scene's build input.
      require(scene.accelerationBytes() > 0 && scene.buildBytes() > 0,
              "Owned acceleration and build storage");
      Queries queries(fixture);
      queries.scene(scene);
      fixture.begin();
      scene.recordBuild(fixture.command);
      queries.record();
      fixture.finish();
      queries.verify();
      // A query, rebuild and another query in one submission exercises incoming
      // query-read/build-write ordering, not just a host fence between builds.
      fixture.begin();
      queries.record();
      scene.recordBuild(fixture.command);
      queries.record();
      fixture.finish();
      queries.verify();
      scene.releaseBuildInputs();
      require(scene.buildBytes() == 0 && scene.accelerationBytes() > 0,
              "Post-build input/scratch retirement");
      fixture.begin();
      queries.record();
      fixture.finish();
      queries.verify();
      const auto kept = scene.handle();
      positions[0][0] = -1;
      const uint32_t invalid[] = {0, 1, 100};
      bool refused = false;
      try {
        scene.initialize(fixture.physical, fixture.device, {positions, invalid});
      } catch (const std::invalid_argument &) {
        refused = true;
      }
      require(refused && scene.handle() == kept, "Invalid replacement preserves an existing ray scene");
      scene.initialize(fixture.physical, fixture.device, {});
      queries.scene(scene);
      fixture.begin();
      scene.recordBuild(fixture.command);
      queries.record();
      fixture.finish();
      queries.verify(true);
      scene.releaseBuildInputs();
    }
  }
  require(validationErrors == 0, "Hardware ray scene emitted Vulkan validation errors");
  std::puts("Hardware ray queries: closest/miss/short/backface/candidate filtering, rebuild, input "
            "retirement and empty scene passed");
} catch (VkResult result) {
  std::fprintf(stderr, "Hardware ray test failed with Vulkan result %d\n", int(result));
  return 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "Hardware ray test failed: %s\n", error.what());
  return 1;
}
