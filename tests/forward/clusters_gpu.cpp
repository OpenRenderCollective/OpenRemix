#include "forward/bindings.h"
#include "forward/shaders/embedded.h"
#include "cluster_shaders/embedded.h"
#include "gpu_fixture.h"
#include <algorithm>
#include <exception>

using namespace remix;
using namespace remix::forward;
using namespace remix::forward::test;
namespace {
  using UInt4 = std::array<uint32_t, 4>;
  struct Gpu {
    VkDevice device;
    Buffer frame, lights, clusters, results;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkPipeline build{}, query{};
    VkDescriptorPool pool{};
    ~Gpu() {
      if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
      if (build) vkDestroyPipeline(device, build, nullptr);
      if (query) vkDestroyPipeline(device, query, nullptr);
      if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
      if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
  };
  VkPipeline pipeline(Gpu &gpu, const uint32_t *code, size_t bytes) {
    const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = bytes, .pCode = code};
    VkShaderModule module{};
    check(vkCreateShaderModule(gpu.device, &moduleInfo, nullptr, &module));
    const VkComputePipelineCreateInfo info{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = gpu.layout};
    VkPipeline result{};
    const auto status = vkCreateComputePipelines(gpu.device, VK_NULL_HANDLE, 1, &info, nullptr, &result);
    vkDestroyShaderModule(gpu.device, module, nullptr);
    check(status);
    return result;
  }
}
int main() try {
  Fixture fixture(false);
  Gpu gpu{fixture.device};
  const VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = uint32_t(std::size(bindings)), .pBindings = bindings};
  check(vkCreateDescriptorSetLayout(gpu.device, &setInfo, nullptr, &gpu.setLayout));
  const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &gpu.setLayout};
  check(vkCreatePipelineLayout(gpu.device, &layoutInfo, nullptr, &gpu.layout));
  gpu.build = pipeline(gpu, clusterShader, sizeof(clusterShader));
  gpu.query = pipeline(gpu, cluster_input::queryShader, sizeof(cluster_input::queryShader));
  gpu.frame.create(fixture.physical, gpu.device, sizeof(FrameData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  gpu.lights.create(fixture.physical, gpu.device, 128 * sizeof(LightData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.clusters.create(fixture.physical, gpu.device, 8 * (64 + clusterHeaderWords) * sizeof(uint32_t),
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.results.create(fixture.physical, gpu.device, 16 * sizeof(UInt4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}};
  const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
  check(vkCreateDescriptorPool(gpu.device, &poolInfo, nullptr, &gpu.pool));
  const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = gpu.pool, .descriptorSetCount = 1, .pSetLayouts = &gpu.setLayout};
  VkDescriptorSet set{};
  check(vkAllocateDescriptorSets(gpu.device, &allocate, &set));
  const VkDescriptorBufferInfo buffers[] = {gpu.frame.descriptor(), gpu.lights.descriptor(),
                                           gpu.clusters.descriptor(), gpu.results.descriptor()};
  VkWriteDescriptorSet writes[4];
  for (uint32_t i = 0; i < 4; ++i)
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
        .dstBinding = bindings[i].binding, .descriptorCount = 1, .descriptorType = bindings[i].descriptorType,
        .pBufferInfo = &buffers[i]};
  vkUpdateDescriptorSets(gpu.device, 4, writes, 0, nullptr);
  uint32_t cases = 0;
  auto run = [&](std::span<const LightData> lights, uint32_t prefix, uint32_t capacity,
                 bool clustered, float sign) {
    FrameData frame{};
    frame.view = frame.inverseProjection = scene::identity();
    frame.view[10] = sign;
    frame.grid = {2, 2, 2, 8};
    frame.depth = {.1f, 10, 0, sign};
    frame.target[2] = uint32_t(lights.size());
    frame.clusterInfo = {capacity, prefix == UINT32_MAX ? 0 : prefix + 1, 0, 0};
    if (clustered) frame.clusterInfo[1] |= clusteredBakedTail;
    gpu.frame.write(&frame, sizeof(frame));
    auto prepared = std::vector<LightData>(lights.begin(), lights.end());
    const float radiusScale = viewRadiusScale(frame.view);
    for (auto &light : prepared)
      light.clusterBounds = clusterSphereBounds(frame.view,
          {light.positionRadius[0], light.positionRadius[1], light.positionRadius[2]}, light.shaping[2], radiusScale);
    gpu.lights.write(prepared.data(), prepared.size() * sizeof(LightData));
    std::vector<uint32_t> cleared(8 * (64 + clusterHeaderWords), 0xdeadbeefu);
    gpu.clusters.write(cleared.data(), cleared.size() * sizeof(uint32_t));
    fixture.begin();
    vkCmdBindDescriptorSets(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.layout, 0, 1, &set, 0, nullptr);
    vkCmdBindPipeline(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.build);
    vkCmdDispatch(fixture.command, 1, 1, 1);
    VkMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    vkCmdBindPipeline(fixture.command, VK_PIPELINE_BIND_POINT_COMPUTE, gpu.query);
    vkCmdDispatch(fixture.command, 1, 1, 1);
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    fixture.finish();
    std::array<UInt4, 16> results;
    gpu.results.read(results.data(), sizeof(results));
    gpu.clusters.read(cleared.data(), cleared.size() * sizeof(uint32_t));
    const uint32_t runtime = prefix == UINT32_MAX ? uint32_t(lights.size()) : prefix;
    for (uint32_t cell = 0; cell < 8; ++cell) {
      const float depthMinimum = cell < 4 ? .1f : 1.f, depthMaximum = cell < 4 ? 1.f : 10.f;
      const Vec3 low{cell % 2 ? 0.f : -1.f, (cell / 2) % 2 ? 0.f : -1.f,
                     sign > 0 ? depthMinimum : -depthMaximum};
      const Vec3 high{cell % 2 ? 1.f : 0.f, (cell / 2) % 2 ? 1.f : 0.f,
                      sign > 0 ? depthMaximum : -depthMinimum};
      std::vector<uint32_t> all, active;
      for (uint32_t i = 0; i < lights.size(); ++i) {
        float distance2 = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          const float centre = lights[i].positionRadius[c] * (c == 2 ? sign : 1);
          const float delta = centre - std::clamp(centre, low[c], high[c]);
          distance2 += delta * delta;
        }
        if (distance2 <= lights[i].shaping[2] * lights[i].shaping[2]) {
          all.push_back(i);
          if (i < runtime) active.push_back(i);
        }
      }
      for (uint32_t receiver = 0; receiver < 2; ++receiver) {
        const bool fallback = receiver == 1 && prefix != UINT32_MAX;
        const auto &selected = fallback ? all : active;
        const bool linear = (fallback && !clustered) || selected.size() > capacity;
        const uint32_t count = linear ? (fallback ? uint32_t(lights.size()) : runtime) : uint32_t(selected.size());
        const UInt4 expected{count, uint32_t(linear), count == 0 ? UINT32_MAX : linear ? 0u : selected.front(),
                             count == 0 ? UINT32_MAX : linear ? count - 1 : selected.back()};
        require(results[cell * 2 + receiver] == expected,
                "Cluster GPU query preserves complete covered/uncovered light selection and overflow");
        if (!linear)
          for (uint32_t i = 0; i < count; ++i)
            require(cleared[cell * (capacity + clusterHeaderWords) + clusterHeaderWords + i] == selected[i],
                    "Stored cluster indices retain sorted late lights and runtime prefix ownership");
      }
    }
    ++cases;
  };
  std::vector<LightData> lights(70);
  for (auto &light : lights) { light.positionRadius = {-.5f, -.5f, .5f, .1f}; light.shaping[2] = .1f; }
  for (float sign : {1.f, -1.f})
    for (uint32_t capacity : {1u, 2u, 64u})
      for (bool clustered : {false, true}) {
        run({}, 0, capacity, clustered, sign);
        run(std::span(lights).first(1), 0, capacity, clustered, sign);
        run(lights, 1, capacity, clustered, sign);
        run(lights, 65, capacity, clustered, sign);
        run(lights, UINT32_MAX, capacity, clustered, sign);
      }
  for (uint32_t i = 0; i < lights.size() - 1; ++i) lights[i].positionRadius[0] = 100;
  run(lights, 1, 64, true, 1);
  require(validationErrors.load() == 0, "Cluster dispatch and receiver queries remain validation-clean");
  std::printf("Clustered baked-tail GPU membership and independent overflow passed (%u cases)\n", cases);
  return 0;
} catch (VkResult result) {
  std::fprintf(stderr, "Cluster GPU test failed with Vulkan status %d\n", int(result));
  return 1;
} catch (const std::exception &error) {
  std::fprintf(stderr, "Cluster GPU test failed: %s\n", error.what());
  return 1;
}
