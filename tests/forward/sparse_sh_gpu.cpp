#include "forward/bake/sparse_sh.h"
#include "forward/bake/grid.h"
#include "forward/bake/spherical_harmonics.h"
#include "forward/half.h"
#include "forward/lightmap/data.h"
#include "gpu_fixture.h"
#include "style_shaders/embedded.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace remix;
using namespace remix::forward;
using namespace remix::forward::test;
namespace b = remix::forward::bake;
namespace s = b::sparse;
namespace lm = remix::forward::lightmap;
int main(int argc, char **argv) try {
  const bool localFactor = argc > 1 && std::strcmp(argv[1], "adaptive-localfactor") == 0;
  const bool stitched = argc > 1 && std::strcmp(argv[1], "adaptive-stitched") == 0;
  const bool depth = argc > 1 && std::strcmp(argv[1], "adaptive-depth") == 0;
  const bool fineFallback = argc > 1 && std::strcmp(argv[1], "adaptive-fallback") == 0;
  const bool refined = depth || stitched || localFactor || fineFallback ||
                       (argc > 1 && std::strcmp(argv[1], "adaptive") == 0);
  const uint32_t probeCount = stitched ? 57 : refined ? 35 : 8;
  Fixture fixture(false);
  struct Gpu {
    VkDevice device;
    Buffer grid, weights, queries, output, staging;
    Image atlas;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    VkSampler sampler{};
    ~Gpu() {
      if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
      if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
      if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
      if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
      if (sampler) vkDestroySampler(device, sampler, nullptr);
    }
  } gpu{fixture.device};
  lm::Layout layout;
  layout.pageSize = 33;
  layout.pages = 2;
  const uint32_t texels = 33 * 33 * 2;
  layout.points.resize(texels);
  layout.owners.resize(texels);
  std::vector<b::Result> surface32(texels), surface97(texels), baseSurface(texels);
  for (uint32_t i = 0; i < texels; ++i) {
    layout.owners[i] = i;
    const uint32_t x = i % 33, y = i / 33 % 33;
    baseSurface[i].irradiance[0] = {.1f, .2f, .3f, 0};
    baseSurface[i].irradiance[2][0] = -.5f;
    if (x < 17 || y < 17) {
      surface32[i].irradiance[0] = {.8f + x * .001f, .02f * y, .03f, 0};
      surface32[i].irradiance[2][0] = x < 16 ? -1.f : .25f;
      surface32[i].irradiance[5][1] = -.04f;
      surface32[i].irradiance[8][2] = .005f;
    }
    if (i >= texels / 2 && x < 17 && y < 17) {
      surface97[i].irradiance[0][1] = .3f;
      surface97[i].irradiance[1][1] = -.2f;
    }
  }
  b::Grid base;
  base.header.originSpacing = {-1,-1,-1,2};
  base.header.dimensions = {2,2,2,8};
  base.header.settings = {.05f,.0001f,1,0};
  if (stitched) {
    base.header.dimensions = {3, 2, 2, probeCount};
    base.header.settings[2] = 5;
    base.lookup.resize(84);
    for (uint32_t node = 0; node < 12; ++node) base.lookup[node] = node + 1;
    base.lookup[12] = b::adaptiveDirectoryMagic;
    base.lookup[13] = 2;
    base.lookup[14] = 84;
    base.lookup[15] = 12;
    base.lookup[16] = b::patchReference(28, 2, 2);
    base.lookup[17] = b::patchReference(56, 2, 1);
    for (uint32_t node = 0; node < 27; ++node) base.lookup[28 + node] = 13 + node;
    uint32_t next = 40;
    for (uint32_t z = 0; z < 3; ++z)
      for (uint32_t y = 0; y < 3; ++y)
        for (uint32_t x = 0; x < 3; ++x)
          base.lookup[56 + (z * 3 + y) * 3 + x] = x ? next++ : 13 + (z * 3 + y) * 3 + 2;
  } else if (refined) {
    base.header.dimensions[3] = probeCount;
    base.header.settings[2] = 5;
    base.lookup.resize(48);
    for (uint32_t node = 0; node < 8; ++node) base.lookup[node] = node + 1;
    base.lookup[8] = b::adaptiveDirectoryMagic;
    base.lookup[9] = localFactor ? 8 : 2;
    base.lookup[10] = 48;
    base.lookup[11] = 8;
    base.lookup[12] = localFactor ? b::patchReference(20, 2) : 20;
    for (uint32_t node = 0; node < 27; ++node) base.lookup[20 + node] = 9 + node;
  } else if (argc > 1) {
    base.header.dimensions = {3, 3, 3, 8};
    base.header.settings[2] = 3;
    base.lookup.resize(27);
    for (uint32_t corner = 0; corner < 8; ++corner) {
      const auto x = (corner & 1) * 2, y = ((corner >> 1) & 1) * 2, z = ((corner >> 2) & 1) * 2;
      base.lookup[(z * 3 + y) * 3 + x] = 8 - corner;
    }
  }
  base.probes.resize(probeCount);
  std::vector<b::Result> probe32(probeCount), probe97(probeCount);
  for (uint32_t i = 0; i < probeCount; ++i) {
    base.probes[i].irradiance[0] = {.8f + i * .025f,.5f,.3f, i & 1 ? -.4f : .4f};
    base.probes[i].irradiance[1][3] = i & 2 ? -.2f : .2f;
    base.probes[i].statistics[3] = 1;
    for (auto &moment : base.probes[i].moments) moment = {1.2f,1.8f,.1f,32};
    probe32[i].irradiance[0][0] = .1f;
    probe32[i].irradiance[2][0] = -1;
    probe97[i].irradiance[0][1] = .25f + i * .02f;
    probe97[i].irradiance[2][1] = .5f;
  }
  // A full nominal fine blend is not proof of supported fine coverage. The
  // production shader must reconstruct the coarse field when all fine records
  // are unusable, including the exact patch centre.
  if (fineFallback)
    for (uint32_t i = 8; i < probeCount; ++i) base.probes[i].statistics[3] = 0;
  s::Builder builder({33,2,probeCount}, 4u << 20);
  builder.add(97, layout, surface97, probe97);
  builder.add(32, layout, surface32, probe32);
  const auto words = builder.serialize();
  const uint32_t styles[] = {32,97};
  s::validate(words, {33,2,probeCount}, styles, 4u << 20);
  if (depth) {
    base.depth.maximumDistance = 10;
    base.depth.records.resize(probeCount);
    for (uint32_t record = 0; record < probeCount; ++record)
      for (uint32_t texel = 0; texel < b::ProbeDepth::texels; ++texel) {
        const float mean = .05f + .003f * texel + .001f * record;
        base.depth.records[record][texel] = uint32_t(half::encode(mean)) |
            uint32_t(half::encode(mean * mean + .0001f)) << 16;
      }
    base.depth.validate(probeCount);
  }
  const size_t baseBytes = b::gridBytes(base);
  std::vector<std::byte> encoded((baseBytes + words.size() * 4 + 15) / 16 * 16);
  const auto publishedHeader = b::publicationHeader(base.header, base.lookup.size());
  std::memcpy(encoded.data(), &publishedHeader, sizeof(publishedHeader));
  std::memcpy(encoded.data() + sizeof(base.header), base.probes.data(), base.probes.size() * sizeof(b::Result));
  if (!base.lookup.empty())
    std::memcpy(encoded.data() + sizeof(base.header) + base.probes.size() * sizeof(b::Result),
                base.lookup.data(), base.lookup.size() * sizeof(uint32_t));
  if (depth) {
    const auto offset = sizeof(b::GridHeader) + base.probes.size() * sizeof(b::Result) + b::lookupBytes(base);
    base.depth.write(std::span(encoded).subspan(offset, base.depth.bytes()));
  }
  std::memcpy(encoded.data() + baseBytes, words.data(), words.size() * 4);
  gpu.grid.create(fixture.physical, fixture.device, encoded.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.grid.write(encoded.data(), encoded.size());
  struct Query { Vec4 coordinate, normal; b::UInt4 control; };
  static_assert(sizeof(Query) == 48);
  std::vector<Query> queries;
  const Vec3 normals[] = {{0,0,1}, {1,0,0}, {.3f,.4f,.5f}};
  for (uint32_t page = 0; page < 2; ++page)
    for (uint32_t y = 0; y < 65; ++y) for (uint32_t x = 0; x < 65; ++x) {
      const auto &n = normals[(x + y) % 3];
      const Vec4 coordinate{(x * .5f + .5f) / 33, (y * .5f + .5f) / 33, float(page), 1};
      queries.push_back({coordinate, {n[0],n[1],n[2],0}, {0,0,0,0}});
      queries.push_back({coordinate, {n[0],n[1],n[2],0}, {3,0,0,0}});
    }
  for (float page : {std::nextafter(1.f, 0.f), std::nextafter(1.f, 2.f)}) {
    const Vec4 coordinate{8.5f/33, 8.5f/33, page, 1};
    queries.push_back({coordinate, {0,0,1,0}, {0,0,0,0}});
    queries.push_back({coordinate, {0,0,1,0}, {3,0,0,0}});
  }
  for (uint32_t i = 0; i < probeCount; ++i) for (const auto &n : normals)
    queries.push_back({{}, {n[0],n[1],n[2],0}, {1,i,0,0}});
  for (float x : {-.5f,0.f,.5f,1.f,2.f,4.f}) for (const auto &n : normals)
    queries.push_back({{x,0,0,0}, {n[0],n[1],n[2],0}, {2,0,0,0}});
  if (stitched) for (float x : {.9999f, 1.f, 1.0001f}) for (const auto &n : normals)
    queries.push_back({{x, 0, 0, 0}, {n[0], n[1], n[2], 0}, {2, 0, 0, 0}});
  queries.push_back({{.5f,.5f,-1,1}, {0,0,1,0}, {0,0,0,0}});
  queries.push_back({{.5f,.5f,0,0}, {0,0,1,0}, {0,0,0,0}});
  gpu.weights.create(fixture.physical, fixture.device, 32 * sizeof(Vec4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  gpu.queries.create(fixture.physical, fixture.device, queries.size() * sizeof(Query), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  gpu.queries.write(queries.data(), queries.size() * sizeof(Query));
  gpu.output.create(fixture.physical, fixture.device, queries.size() * sizeof(Vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  const auto dense = lm::pixels(layout, baseSurface);
  gpu.atlas.create(fixture.physical, fixture.device, {33,33}, VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT,
      VK_IMAGE_ASPECT_COLOR_BIT, 2 * lm::atlasLayers);
  gpu.staging.create(fixture.physical, fixture.device, dense.size() * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  gpu.staging.write(dense.data(), dense.size() * 2);
  const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
  check(vkCreateSampler(fixture.device, &sampler, nullptr, &gpu.sampler));
  const VkDescriptorSetLayoutBinding bindings[] = {
      {0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
      {1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
      {2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
      {3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
      {6,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
  const VkDescriptorSetLayoutCreateInfo setInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 5, .pBindings = bindings};
  check(vkCreateDescriptorSetLayout(fixture.device, &setInfo, nullptr, &gpu.setLayout));
  const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
  const VkPipelineLayoutCreateInfo layoutInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &gpu.setLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
  check(vkCreatePipelineLayout(fixture.device, &layoutInfo, nullptr, &gpu.layout));
  VkShaderModule module;
  const VkShaderModuleCreateInfo moduleInfo{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(style_input::queryShader), .pCode = style_input::queryShader};
  check(vkCreateShaderModule(fixture.device, &moduleInfo, nullptr, &module));
  const VkComputePipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"}, .layout = gpu.layout};
  const auto created = vkCreateComputePipelines(fixture.device,VK_NULL_HANDLE,1,&pipeline,nullptr,&gpu.pipeline);
  vkDestroyShaderModule(fixture.device,module,nullptr);
  check(created);
  const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1}};
  const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = sizes};
  check(vkCreateDescriptorPool(fixture.device,&pool,nullptr,&gpu.pool));
  VkDescriptorSet set;
  const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = gpu.pool,.descriptorSetCount = 1,.pSetLayouts = &gpu.setLayout};
  check(vkAllocateDescriptorSets(fixture.device,&allocate,&set));
  const VkDescriptorBufferInfo buffers[] = {gpu.grid.descriptor(),gpu.weights.descriptor(),gpu.queries.descriptor(),gpu.output.descriptor()};
  VkWriteDescriptorSet writes[5]{};
  for (uint32_t i = 0; i < 4; ++i)
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet = set,.dstBinding = i,
        .descriptorCount = 1,.descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffers[i]};
  const VkDescriptorImageInfo image{gpu.sampler,gpu.atlas.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  writes[4] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet = set,.dstBinding = 6,
      .descriptorCount = 1,.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,.pImageInfo = &image};
  vkUpdateDescriptorSets(fixture.device,5,writes,0,nullptr);
  fixture.begin();
  VkImageMemoryBarrier barrier{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,.image = gpu.atlas.handle,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,2 * lm::atlasLayers}};
  vkCmdPipelineBarrier(fixture.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
      0,0,nullptr,0,nullptr,1,&barrier);
  const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,2 * lm::atlasLayers},
      .imageExtent = {33,33,1}};
  vkCmdCopyBufferToImage(fixture.command,gpu.staging.handle,gpu.atlas.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  vkCmdPipelineBarrier(fixture.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0,0,nullptr,0,nullptr,1,&barrier);
  fixture.finish();
  auto baseSurfaceAt = [&](Vec4 coordinate, Vec3 normal) {
    Vec3 value{};
    if (coordinate[3] < .5f) return value;
    const auto sh = b::irradianceBasis(normal);
    // The fixture's base field is constant; the production texture sampler
    // still exercises real half-format surface lookup and receiver clamping.
    for (uint32_t j = 0; j < 9; ++j) for (uint32_t c = 0; c < 3; ++c)
      value[c] += sh[j] * half::decode(half::encode(baseSurface[0].irradiance[j][c]));
    return value;
  };
  float maximumError = 0;
  const Vec4 scenarios[] = {{0,0,1,0},{1,0,1,0},{.5f,.75f,1,0},{1,1,1,0},{1,1,0,0}};
  for (const auto &scenario : scenarios) {
    std::array<Vec4,32> weights{};
    weights[32/4][32%4] = scenario[0];
    weights[97/4][97%4] = scenario[1];
    gpu.weights.write(weights.data(),sizeof(weights));
    auto header = b::publicationHeader(base.header, base.lookup.size());
    header.settings[3] = b::publicationSettings(0, scenario[2] != 0, depth);
    gpu.grid.write(&header,sizeof(header));
    const b::StyleScale scales[] = {{32,scenario[2] ? scenario[0] : 0,b::StyleStatus::Ready},
                                   {97,scenario[2] ? scenario[1] : 0,b::StyleStatus::Ready}};
    auto combined = base;
    for (uint32_t i = 0; i < probeCount; ++i) for (const auto &scale : scales) {
      const auto coefficients = s::probe(words,scale.style,i);
      for (uint32_t j = 0; j < 9; ++j) for (uint32_t c = 0; c < 3; ++c)
        combined.probes[i].irradiance[j][c] += scale.value * coefficients[j][c];
    }
    fixture.begin();
    vkCmdBindPipeline(fixture.command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu.pipeline);
    vkCmdBindDescriptorSets(fixture.command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu.layout,0,1,&set,0,nullptr);
    const uint32_t count = uint32_t(queries.size());
    vkCmdPushConstants(fixture.command,gpu.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&count);
    vkCmdDispatch(fixture.command,(count+63)/64,1,1);
    const VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,.dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(fixture.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&host,0,nullptr,0,nullptr);
    fixture.finish();
    std::vector<Vec4> values(queries.size());
    gpu.output.read(values.data(),values.size()*sizeof(Vec4));
    for (size_t i = 0; i < queries.size(); ++i) {
      const auto &q = queries[i];
      const Vec3 normal = normalized({q.normal[0],q.normal[1],q.normal[2]});
      Vec3 expected{};
      bool found = true;
      if (q.control[0] == 0 || q.control[0] == 3) {
        expected = s::surfaceIrradiance(words,scales,q.coordinate,normal);
        if (q.control[0] == 3) {
          found = q.coordinate[3] >= .5f;
          const auto initial = baseSurfaceAt(q.coordinate,normal);
          for (uint32_t c = 0; c < 3; ++c) expected[c] = std::max(0.f,initial[c]+expected[c]);
        }
      } else if (q.control[0] == 1) {
        const auto sh = b::irradianceBasis(normal);
        for (const auto &scale : scales) {
          const auto coefficients = s::probe(words,scale.style,q.control[1]);
          for (uint32_t j = 0; j < 9; ++j) for (uint32_t c = 0; c < 3; ++c)
            expected[c] += scale.value * sh[j] * coefficients[j][c];
        }
      } else {
        const auto value = b::sample(combined,{q.coordinate[0],q.coordinate[1],q.coordinate[2]},normal);
        found = bool(value);
        if (value) expected = *value;
      }
      require((values[i][3] > .5f) == found,"Sparse GPU sampling preserves receiver coverage");
      for (uint32_t c = 0; c < 3; ++c) {
        const float error = std::abs(values[i][c]-expected[c]);
        maximumError = std::max(maximumError,error);
        if (error >= 2e-5f) std::fprintf(stderr,"Sparse query %zu mode%u channel%u expected%.9g actual%.9g\n",i,q.control[0],c,expected[c],values[i][c]);
        require(error < 2e-5f,"Production sparse shader agrees with packed surface/probe reconstruction");
      }
    }
  }
  require(validationErrors == 0,"Sparse style storage and lookup are Vulkan validation clean");
  std::printf("Sparse GPU style SH: %zu samples across5states, tile/page boundaries, signed mix, relocated probes; maxerror %.9g\n",
              queries.size()*std::size(scenarios),maximumError);
} catch (VkResult error) {
  std::fprintf(stderr,"Sparse GPU style test failed: %d\n",int(error));
  return 1;
}
