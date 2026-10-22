#pragma once
#include "forward/probes/image_asset.h"
#include "forward/bake/probe_visibility.h"
#include "forward/half.h"
#include "gpu_fixture.h"
#include "shaders/embedded.h"
#include <cmath>

namespace remix::forward::test {
  // Runs the production probe lookup on the GPU against its CPU reference.
  // Uses the production HDR image owner and its compact projection table.
  inline void probeQueries(Fixture &fixture, const probes::Data &data, uint32_t skyCase = 0,
                            bool directional = false, bool indexed = false, bool depth = false) {
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
        {{-.5f, .2f, 0, 2}, {0, 1, 0, 0}}, {{4, 0, 0, 2}, {0, 0, 1, 0}},
        {{0, 0, 0, 3}, {0, 0, 1, 0}}, {{.5f, 0, 0, 3}, {1, 0, 0, 0}},
        {{-.5f, .2f, 0, 3}, {0, 1, 0, 0}}, {{4, 0, 0, 3}, {0, 0, 1, 0}},
        {{0, 0, 0, 4}, {0, 0, 1, 0}}, {{.5f, 0, 0, 4}, {1, 0, 0, 0}},
        {{4, 0, 0, 4}, {0, 0, 1, 0}}, {{0, 0, 0, 5}, {0, 0, 1, 0}},
        {{0, 0, 0, 6}, {0, 0, 1, 0}}, {{0, 0, 0, 7}, {0, 0, 1, 0}},
        // The aggregate contributes C once; the residual must leave C*V.
        {{2, 3, 5, 8}, {1, 1, 0, 0}}, {{2, 3, 5, 8}, {0, 1, 0, 0}},
        {{2, 3, 5, 8}, {.25f, 1, 0, 0}}, {{2, 3, 5, 8}, {.75f, 1, 0, 0}},
        // Missing aggregate receiver coverage keeps ordinary direct lighting.
        {{2, 3, 5, 8}, {1, 0, 0, 0}}, {{2, 3, 5, 8}, {0, 0, 0, 0}},
        {{2, 3, 5, 8}, {.25f, 0, 0, 0}}, {{0, 0, 0, 8}, {.5f, 1, 0, 0}},
        {{0, 0, 0, 9}, {0, 0, 1, 0}}, {{.5f, 0, 0, 9}, {1, 0, 0, 0}},
        {{4, 0, 0, 9}, {0, 0, 1, 0}}, {{0, 0, 0, 9}, {0, 0, -1, 0}},
        {{0, 0, 0, 10}, {0, 0, 1, 0}}, {{0, 0, 0, 11}, {0, 0, 1, 0}}};
    struct Gpu {
      VkDevice device;
      Buffer input, output, sky;
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
    probes::ImageAsset asset(fixture.physical, fixture.device, data);
    auto quantized = data;
    for (auto &pixel : quantized.texels)
      for (uint32_t c = 0; c < 3; ++c) pixel[c] = half::decode(half::encode(pixel[c]));
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
      probe.irradiance[7][3] = skyCase == 1 && i == 0 ? 0.f : 1.f;
      probe.irradiance[8][3] = float(i) / 7.f;
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
    std::vector<bake::VisibilityTask> pairs;
    std::vector<float> masks;
    for (uint32_t probe = 0; probe < 8; ++probe) {
      for (uint32_t light = 0; light < 66; ++light) {
        if (light == 1 && probe == 0) continue;
        pairs.push_back({probe, light, 0});
        masks.push_back(float(probe) / 7.f);
      }
    }
    bake::ProbeVisibility local(8, 66, pairs, 32768, directional);
    if (directional) {
      std::vector<bake::VisibilityResult> results(pairs.size());
      for (size_t i = 0; i < pairs.size(); ++i) {
        results[i].visibility = masks[i];
        // One fully covered light uses the shared black field. Other lights
        // retain signed coefficients and distinct amplitudes across corners.
        if (pairs[i].light != 64) {
          results[i].irradiance[0] = .5f + float(pairs[i].point) * .1f + float(pairs[i].light) * .003f;
          results[i].irradiance[2] = -.2f;
        }
      }
      local.observe(pairs, results);
    } else local.observe(pairs, masks);
    const auto localWords = local.words();
    const uint32_t lookup[] = {8, 7, 6, 5, 4, 3, 2, 1};
    if (indexed) sky.header.settings[2] += 2;
    // Exercise offsets beyond the old 128 MiB numeric-float limit on capable devices.
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(fixture.physical, &properties);
    const size_t largeOffset = sizeof(bake::GridHeader) + (size_t(1) << 27) + 16;
    const bool large = skyCase == 4 && directional &&
        largeOffset + localWords.size() * sizeof(uint32_t) <= properties.limits.maxStorageBufferRange;
    bake::ProbeDepth depthAsset;
    if (depth) {
      depthAsset.maximumDistance = 10;
      depthAsset.records.resize(8);
      for (uint32_t probe = 0; probe < 8; ++probe)
        for (uint32_t texel = 0; texel < bake::ProbeDepth::texels; ++texel) {
          const float mean = .05f + .003f * texel + .001f * probe;
          depthAsset.records[probe][texel] = uint32_t(half::encode(mean)) |
              uint32_t(half::encode(mean * mean + .0001f)) << 16;
        }
    }
    const size_t depthOffset = sizeof(sky) + (indexed ? sizeof(lookup) : 0);
    const size_t localOffset = large ? largeOffset : depthOffset + depthAsset.bytes();
    sky.header.settings[3] = bake::publicationSettings((localOffset - sizeof(bake::GridHeader)) / 16, large, depth);
    gpu.sky.create(fixture.physical, fixture.device, localOffset + localWords.size() * sizeof(uint32_t),
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto publishedSky = sky;
    publishedSky.header = bake::publicationHeader(sky.header, indexed ? std::size(lookup) : 0);
    gpu.sky.write(&publishedSky, sizeof(publishedSky));
    if (indexed) gpu.sky.write(lookup, sizeof(lookup), sizeof(sky));
    if (depth) {
      std::vector<std::byte> bytes(depthAsset.bytes());
      depthAsset.write(bytes);
      gpu.sky.write(bytes.data(), bytes.size(), depthOffset);
    }
    gpu.sky.write(localWords.data(), localWords.size() * sizeof(uint32_t), localOffset);
    bake::Grid reference;
    reference.header = sky.header;
    reference.probes.assign(std::begin(sky.probes), std::end(sky.probes));
    reference.depth = depthAsset;
    if (indexed) reference.lookup.assign(std::begin(lookup), std::end(lookup));
    gpu.input.create(fixture.physical, fixture.device, sizeof(queries), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    gpu.input.write(queries, sizeof(queries));
    gpu.output.create(fixture.physical, fixture.device, std::size(queries) * sizeof(Vec4),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    const VkDescriptorSetLayoutCreateInfo setInfo{.sType =
                                                      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = 5,
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
    const VkDescriptorPoolSize size[]{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    const VkDescriptorPoolCreateInfo poolInfo{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                              .maxSets = 1,
                                              .poolSizeCount = 2,
                                              .pPoolSizes = size};
    check(vkCreateDescriptorPool(fixture.device, &poolInfo, nullptr, &gpu.pool));
    VkDescriptorSet set;
    const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = gpu.pool,
                                               .descriptorSetCount = 1,
                                               .pSetLayouts = &gpu.setLayout};
    check(vkAllocateDescriptorSets(fixture.device, &allocate, &set));
    const VkDescriptorBufferInfo buffers[] = {gpu.input.descriptor(), gpu.output.descriptor(),
                                              asset.metadata(), gpu.sky.descriptor()};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t i = 0; i < 4; ++i)
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = set,
                   .dstBinding = bindings[i].binding,
                   .descriptorCount = 1,
                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .pBufferInfo = &buffers[i]};
    const auto radiance = asset.radiance();
    writes[4] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 14,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &radiance};
    vkUpdateDescriptorSets(fixture.device, 5, writes, 0, nullptr);
    fixture.begin();
    asset.recordUpload(fixture.command);
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
    asset.releaseUpload();
    Vec4 values[std::size(queries)];
    gpu.output.read(values, sizeof(values));
    for (uint32_t i = 0; i < count; ++i) {
      const auto &q = queries[i];
      if (q.position[3] >= 9) {
        const uint32_t light = q.position[3] == 10 ? 1u : q.position[3] == 11 ? 64u : 0u;
        const Vec3 position{q.position[0], q.position[1], q.position[2]};
        const Vec3 normal{q.reflected[0], q.reflected[1], q.reflected[2]};
        auto directReference = reference;
        for (uint32_t probe = 0; probe < 8; ++probe) {
          auto &value = directReference.probes[probe];
          auto coefficients = local.sampleDirect(probe, light);
          // Compare shader decoding/interpolation against the packed CPU
          // reference. Full-precision quantization bounds have separate tests.
          if (coefficients)
            *coefficients = bake::unpackUnitDirect(bake::packUnitDirect(*coefficients));
          if (!local.sample(probe, light)) value.statistics[3] = 0;
          for (uint32_t c = 0; c < 9; ++c) {
            const float scalar = coefficients ? (*coefficients)[c] : 0;
            for (uint32_t rgb = 0; rgb < 3; ++rgb) value.irradiance[c][rgb] = scalar;
          }
        }
        auto expected = bake::sample(directReference, position, normal);
        const auto stencil = bake::probeStencil(reference, position, normal);
        for (uint32_t corner = 0; corner < stencil.count; ++corner)
          if (stencil.weights[corner] > 1e-6f && !local.sample(stencil.indices[corner], light)) expected.reset();
        require((values[i][3] > .5f) == bool(expected), "Directional probe response preserves receiver coverage");
        if (expected) require(directional ? std::abs(values[i][1] - (*expected)[0]) < 2e-5f : values[i][1] == -1,
                              "Indexed directional fields match packed CPU interpolation, including black fields; scalar caches retain unknown SH");
        continue;
      }
      if (q.position[3] == 8) {
        const bool aggregate = q.reflected[1] > .5f;
        for (uint32_t c = 0; c < 3; ++c) {
          const float baked = aggregate ? q.position[c] : 0;
          require(std::abs(baked + values[i][c] - q.position[c] * q.reflected[0]) < 1e-6f,
                  "Aggregate plus GPU mixed-shadow residual equals visible diffuse without double counting");
        }
        continue;
      }
      if (q.position[3] >= 4) {
        auto localReference = reference;
        for (auto &probe : localReference.probes) probe.irradiance[7][3] = 1;
        if (q.position[3] == 5) localReference.probes[0].irradiance[7][3] = 0;
        auto expected = bake::sampleSunVisibility(localReference, {q.position[0], q.position[1], q.position[2]},
                                                   {q.reflected[0], q.reflected[1], q.reflected[2]});
        if (q.position[3] == 6) expected.reset();
        require((values[i][3] > .5f) == bool(expected), "Local probe masks preserve missing and stale identity fallback");
        if (expected) require(std::abs(values[i][0] - *expected) < 2e-5f,
                             "Sparse local masks match wall-aware and relocated CPU interpolation");
        continue;
      }
      if (q.position[3] == 3) {
        const auto expected = bake::sampleSunVisibility(reference, {q.position[0], q.position[1], q.position[2]},
                                                        {q.reflected[0], q.reflected[1], q.reflected[2]});
        require((values[i][3] > .5f) == bool(expected),
                "GPU probe sunlight preserves missing, relocated and wall-occluded coverage");
        if (expected)
          require(std::abs(values[i][0] - *expected) < 2e-5f,
                  "GPU static sunlight interpolation matches CPU weights without adding irradiance");
        continue;
      }
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
        const auto stencil = bake::probeStencil(reference,
            {q.position[0], q.position[1], q.position[2]}, {0, 1, 0});
        // Every fixture corner has the same sky moments. Receiver visibility
        // may remove all support, including in otherwise open sky cases.
        float support = 0;
        for (uint32_t corner = 0; corner < stencil.count; ++corner) support += stencil.weights[corner];
        const float expected = (skyCase == 0 || skyCase == 4) && support >= 1e-4f ? 1.f : 0.f;
        if (std::abs(values[i][0] - expected) >= 1e-5f)
          std::fprintf(stderr, "Sky case=%u indexed=%d depth=%d query=%u support=%u expected=%g actual=%g\n",
              skyCase, indexed, depth, i, stencil.count, expected, values[i][0]);
        require(std::abs(values[i][0] - expected) < 1e-5f,
                "Sky visibility accepts open space and rejects closed, invalid, occluded and unknown space");
        continue;
      }
      const auto expected = probes::sample(quantized, {q.position[0], q.position[1], q.position[2]},
                                           {q.reflected[0], q.reflected[1], q.reflected[2]}, q.reflected[3]);
      require((values[i][3] > .5f) == bool(expected), "GPU probe influence matches the CPU reference");
      if (expected)
        for (uint32_t c = 0; c < 3; ++c)
          require(std::abs(values[i][c] - (*expected)[c]) < 2e-4f,
                  "GPU cube projection/filter matches the CPU reference");
    }
  }
} // namespace remix::forward::test
