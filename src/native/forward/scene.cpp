#include "forward/bake/styles.h"
#include "forward/gpu.h"
#include "forward/material.h"
#include "forward/probes/capture_dump.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>

namespace remix::forward {
  std::shared_ptr<Renderer::Gpu::Geometry> Renderer::Gpu::geometry(const scene::Draw &draw) {
    const auto &surface = draw.surface;
    const bool skinned = surface->bonesPerVertex != 0;
    if (!skinned)
      if (auto found = geometries.find(surface.get());
          found != geometries.end() && !found->second->source.expired())
        return found->second;
    std::shared_ptr<Geometry> result;
    if (skinned) {
      auto &frame = frames[slot];
      if (frame.posedUsed == frame.posedGeometry.size())
        frame.posedGeometry.push_back(std::make_shared<Geometry>());
      result = frame.posedGeometry[frame.posedUsed++];
    } else {
      result = std::make_shared<Geometry>();
    }
    result->source = surface;
    result->bounds = {};
    std::vector<scene::Vertex> vertices;
    auto input = std::span<const scene::Vertex>(surface->vertices);
    if (skinned) {
      vertices = surface->vertices;
      std::vector<Matrix> normals;
      normals.reserve(draw.bones->size());
      for (const auto &bone : *draw.bones)
        normals.push_back(normalMatrix(bone));
      for (size_t i = 0; i < vertices.size(); ++i) {
        auto &vertex = vertices[i];
        std::fill_n(vertex.position, 3, 0.f);
        std::fill_n(vertex.normal, 3, 0.f);
        const auto &original = surface->vertices[i];
        for (uint32_t b = 0; b < surface->bonesPerVertex; ++b) {
          const size_t index = i * surface->bonesPerVertex + b;
          const uint32_t bone = surface->bones[index];
          const auto p = transformPoint((*draw.bones)[bone],
                                        {original.position[0], original.position[1], original.position[2]});
          const auto n =
              transformVector(normals[bone], {original.normal[0], original.normal[1], original.normal[2]});
          for (int c = 0; c < 3; ++c) {
            vertex.position[c] += p[c] * surface->weights[index];
            vertex.normal[c] += n[c] * surface->weights[index];
          }
        }
      }
      input = vertices;
    }
    for (const auto &vertex : input)
      result->bounds.include({vertex.position[0], vertex.position[1], vertex.position[2]});
    ensure(result->vertices, input.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    result->vertices->write(input.data(), input.size_bytes());
    ensure(result->indices, surface->indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    result->indices->write(surface->indices.data(), surface->indices.size() * sizeof(uint32_t));
    if (!skinned)
      geometries[surface.get()] = result;
    return result;
  }
  const Bounds &Renderer::Gpu::frozenShadowBounds() {
    const auto &source = shadowPlanner.source();
    if (!source) {
      staticBounds = {};
      staticBoundsSource.reset();
    } else if (staticBoundsSource.lock() != source) {
      Bounds next;
      for (const auto &draw : source->draws) {
        if (!accepts(draw) || (draw.indexCount == 0 && draw.surface->indices.empty()))
          continue;
        next.merge(transformBounds(geometry(draw)->bounds, draw.model));
      }
      staticBounds = next;
      staticBoundsSource = source;
    }
    return staticBounds;
  }
  void Renderer::Gpu::prepareFrame(VkCommandBuffer command, RasterRenderer &resources) {
    Statistics::CpuRecording::Preparation cpu;
    auto began = std::chrono::steady_clock::now();
    auto step = [&](double &milliseconds) {
      const auto now = std::chrono::steady_clock::now();
      milliseconds = std::chrono::duration<double, std::milli>(now - began).count();
      began = now;
    };
    auto &frame = frames[slot];
    const auto sun = resources.light();
    frame.commands.clear();
    frame.staticCommands.clear();
    frame.terrainCommands.clear();
    frame.waterCommands.clear();
    frame.waterComposed = false;
    frame.waterMedium = false;
    auto sources = resources.sphereLights();
    std::sort(sources.begin(), sources.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    const auto eye = transformPoint(scene::inverse(resources.view()), {0, 0, 0});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count();
    shadowSelection = shadowPlanner.update(sources, eye, options.localShadows ? options.localShadowLights : 0,
                                           options.lightCutoff, options.localShadowDistance,
                                           options.sceneScale, resources.bakeScene(), seconds,
                                           options.localShadowFade);
    step(cpu.selection);
    std::vector<DrawData> draws;
    std::vector<water::MaterialData> waterMaterials;
    Bounds casters;
    auto append = [&](const scene::Draw &draw, std::vector<Command> &commands, bool staticOnly, bool liquid = false) {
      if (!(liquid ? water::accepts(draw) : accepts(draw) || (!staticOnly && acceptsTerrainLayer(draw))))
        return;
      const uint32_t count = draw.indexCount ? draw.indexCount : uint32_t(draw.surface->indices.size());
      if (!count)
        return;
      Command out;
      out.geometry = geometry(draw);
      out.source = draw;
      out.captured = !liquid && (staticOnly || shadowPlanner.captured(draw));
      if (!staticOnly && !liquid)
        if (const auto &volume = baking->volume(); volume && volume->surfaces)
          out.lightmap = volume->surfaces->find(draw);
      out.index = uint32_t(draws.size());
      out.count = count;
      out.worldBounds = transformBounds(out.geometry->bounds, draw.model);
      if (!liquid) casters.merge(out.worldBounds);
      if (liquid) {
        out.waterIndex = uint32_t(waterMaterials.size());
        waterMaterials.push_back(water::materialData(*draw.material, seconds));
      }
      draws.push_back(materialData(draw, seconds));
      commands.push_back(std::move(out));
    };
    for (const auto &draw : resources.draws()) {
      const bool liquid = waterPass && water::accepts(draw);
      append(draw, liquid ? frame.waterCommands
                   : acceptsTerrainLayer(draw) ? frame.terrainCommands : frame.commands, false, liquid);
    }
    // Complete frozen bounds still fit the cascades even when no static draw
    // descriptors or material records need preparing for this frame.
    if (options.shadows && sun.isDistant)
      casters.merge(frozenShadowBounds());
    step(cpu.draws);
    auto &data = frame.data;
    data = {};
    if (const auto &volume = baking->volume(); volume && volume->styledIndirect && volume->mixedLights) {
      for (const auto &scale : bake::styleScales(*volume->mixedLights->source, sources))
        if (scale.status == bake::StyleStatus::Ready)
          data.styleScales[scale.style / 4][scale.style % 4] = scale.value;
    }
    if (baking->volume())
      data.occlusion = baking->volume()->occlusionParameters;
    data.camera = resources.camera();
    data.view = resources.view();
    // The shared camera already includes Vulkan framebuffer Y. Reconstruction
    // from gl_FragCoord must invert that same projection, without flipping it
    // back to the host convention (which mirrors sky, water and screen rays).
    data.inverseProjection = scene::inverse(resources.projection());
    const auto position = transformPoint(scene::inverse(data.view), {0, 0, 0});
    std::copy(position.begin(), position.end(), data.cameraPosition.begin());
    const auto direction = normalized({sun.direction[0], sun.direction[1], sun.direction[2]});
    std::copy(direction.begin(), direction.end(), data.sunDirection.begin());
    std::copy_n(sun.radiance, 3, data.sunRadiance.begin());
    if (const auto &volume = baking->volume(); volume && options.shadows && matchesSun(volume->sun, sun))
      data.sunDirection[3] = 1; // lightmap/probe receivers may use matching static visibility
    data.sunRadiance[3] = std::sin(sun.angularDiameterDegrees * .008726646259971648f);
    data.ambientExposure = {options.ambient, options.ambient, options.ambient, options.exposure};
    const auto range = depthRange(resources.projection());
    data.depth = {range.nearPlane, range.farPlane,
                  float(clusterSlices) / std::log(range.farPlane / range.nearPlane), range.sign};
    const uint32_t x = std::max(1u, (extent.width + 63) / 64), y = std::max(1u, (extent.height + 63) / 64);
    data.grid = {x, y, clusterSlices, x * y * clusterSlices};
    const auto cascades =
        shadowCascades(data.camera, data.view, range, direction, casters,
                       options.shadowDistance / options.sceneScale, options.shadowResolution);
    data.shadows = cascades.matrices;
    data.splits = cascades.splits;
    std::vector<LightData> lights;
    data.localSettings = {
        float(options.localShadowResolution), options.localShadowBias / options.sceneScale,
        options.shadowBias / options.sceneScale,
        float(options.baking.directDiffuse && baking->volume() && baking->volume()->directDiffuse)};
    for (uint32_t i = 0; i < shadows::maximumLights; ++i) {
      if (!shadowSelection.active[i])
        continue;
      const auto &view = shadowSelection.views[i];
      data.localPositions[i] = {view.position[0], view.position[1], view.position[2], view.farPlane};
      std::copy(view.matrices.begin(), view.matrices.end(), data.localShadows.begin() + i * shadows::faces);
    }
    for (uint32_t sourceIndex = 0; sourceIndex < sources.size(); ++sourceIndex) {
      const auto &source = sources[sourceIndex];
      const float radius = influenceRadius(source, options.lightCutoff);
      if (!std::isfinite(radius) || radius <= 0)
        continue;
      LightData light;
      std::copy_n(source.position, 3, light.positionRadius.begin());
      light.positionRadius[3] = source.radius;
      std::copy_n(source.radiance, 3, light.radiance.begin());
      light.radiance[3] = float(source.triangle);
      std::copy_n(source.direction, 3, light.directionCone.begin());
      light.directionCone[3] = source.coneCos;
      light.shaping = {source.softness, source.focus, radius, 0};
      if (const auto &volume = baking->volume(); volume && volume->mixedLights) {
        const auto captured = volume->mixedLights->match(source);
        if (captured != mixed::none) {
          light.shaping[3] = float(captured + 1);
          light.corners[2][3] = float(volume->mixedLights->bakedDiffuse(captured));
          if (light.corners[2][3] > 0 && !volume->mixedLights->retainsVisibility(captured))
            light.corners[2][3] = 2; // fully baked; fallback remains available outside baked coverage
        }
      }
      for (int i = 0; i < 3; ++i)
        std::copy_n(source.corners[i], 3, light.corners[i].begin());
      if (const auto slot = shadowSelection.slots[sourceIndex]; slot != UINT32_MAX) {
        light.corners[0][3] = float(slot + 1);
        light.corners[1][3] = 1 - shadowSelection.strength[slot];
      }
      lights.push_back(light);
    }
    data.target = {extent.width, extent.height, uint32_t(lights.size()),
                   uint32_t(options.shadows && sun.isDistant)};
    frame.staticSunDirty = sunCache.update(shadowPlanner.source(), data.target[3] != 0, data.shadows);
    const bool refreshLocal = std::any_of(shadowSelection.dirty.begin(), shadowSelection.dirty.end(),
                                         [](bool dirty) { return dirty; });
    const bool refreshSun = data.target[3] && std::any_of(frame.staticSunDirty.begin(), frame.staticSunDirty.end(),
                                                         [](bool dirty) { return dirty; });
    if ((refreshLocal || refreshSun) && shadowPlanner.source())
      for (const auto &draw : shadowPlanner.source()->draws)
        append(draw, frame.staticCommands, true);
    step(cpu.lighting);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    data.clusterInfo[0] = clusterListCapacity(uint32_t(lights.size()), data.grid[3],
        properties.limits.maxStorageBufferRange, options.clusterLights);
    statistics.clusterLightCapacity = data.clusterInfo[0];
    const VkDeviceSize clusterBytes = VkDeviceSize(data.grid[3]) * (data.clusterInfo[0] + 1) * sizeof(uint32_t);
    for (const auto bytes : {clusterBytes, VkDeviceSize(lights.size() * sizeof(LightData)),
                             VkDeviceSize(draws.size() * sizeof(DrawData))})
      if (bytes > properties.limits.maxStorageBufferRange)
        throw VK_ERROR_TOO_MANY_OBJECTS;
    ensure(frame.constants, sizeof(data), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    ensure(frame.lights, lights.size() * sizeof(LightData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    ensure(frame.draws, draws.size() * sizeof(DrawData), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    // Only compute writes and fragment shading reads this list. Keeping it
    // GPU-local avoids scattered writes into host-visible memory on discrete GPUs.
    ensure(frame.clusters, clusterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, Buffer::Memory::Device);
    frame.constants->write(&data, sizeof(data));
    frame.lights->write(lights.data(), lights.size() * sizeof(LightData));
    frame.draws->write(draws.data(), draws.size() * sizeof(DrawData));
    probes::dumpFrameData(".world-frame.bin", &data, sizeof(data));
    probes::dumpFrameData(".draws.bin", draws.data(), draws.size() * sizeof(DrawData));
    probes::dumpFrameData(".lights.bin", lights.data(), lights.size() * sizeof(LightData));
    const uint32_t capacity = uint32_t(frame.commands.size() + frame.staticCommands.size() +
                                      frame.terrainCommands.size() + frame.waterCommands.size()) + 2;
    if (!frame.pool || frame.poolCapacity < capacity) {
      if (frame.pool)
        vkDestroyDescriptorPool(device, frame.pool, nullptr);
      frame.pool = VK_NULL_HANDLE;
      frame.poolCapacity = std::max(64u, capacity);
      const VkDescriptorPoolSize sizes[] = {
          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, options.water ? 3u : 2u},
          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frame.poolCapacity * 5}};
      const VkDescriptorPoolCreateInfo info{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                            .maxSets = frame.poolCapacity,
                                            .poolSizeCount = 3,
                                            .pPoolSizes = sizes};
      check(vkCreateDescriptorPool(device, &info, nullptr, &frame.pool));
    }
    frame.scene = allocate(frame, sceneLayout);
    frame.composite = allocate(frame, compositeSetLayout);
    prepareSky(command, resources);
    if (waterPass) {
      water::Parameters parameters;
      parameters.projection = scene::inverse(data.inverseProjection);
      parameters.inverseCamera = scene::inverse(data.camera);
      parameters.cameraMedium = resources.cameraMedium();
      frame.waterMedium = parameters.cameraMedium[3] > 0;
      parameters.target = {float(extent.width), float(extent.height), options.waterDistance / options.sceneScale,
                            .002f / options.sceneScale};
      parameters.settings = {options.waterThickness / options.sceneScale,
                              parameters.cameraMedium[3] > 0 ? 1.f : 0.f, options.sceneScale, 0};
      const auto up = transformVector(resources.dome().worldToLocal, {0, 0, 1});
      std::copy(up.begin(), up.end(), parameters.domeUp.begin());
      const float light = sun.isDistant ? std::max(-direction[2], 0.f) / 3.14159265359f : 0;
      for (uint32_t c = 0; c < 3; ++c) parameters.sunFog[c] = sun.radiance[c] * light;
      const auto screen = reflections ? reflections->screenInputs() : Reflections::ScreenInputs{};
      parameters.reflectionTrace = {options.ssrDistance / options.sceneScale,
          options.ssrThickness / options.sceneScale, .002f / options.sceneScale, options.ssrRoughness};
      parameters.reflectionConfig = {uint32_t(samples), options.ssrScale,
          screen.depth.imageView ? options.ssrSteps : 0u,
          uint32_t(std::bit_width(std::max(extent.width, extent.height)))};
      waterPass->prepare(slot, parameters, waterMaterials,
                          {baking->reflectionDescriptor(), frame.skyConstants->descriptor(), frame.skyImage,
                           screen.depth, screen.normals, screen.color});
      const auto medium = waterPass->parameters(slot);
      const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = frame.composite, .dstBinding = 5, .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &medium};
      vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
    const VkDescriptorBufferInfo buffers[] = {frame.constants->descriptor(), frame.lights->descriptor(),
                                              frame.clusters->descriptor(), frame.draws->descriptor()};
    VkWriteDescriptorSet writes[14]{};
    for (uint32_t i = 0; i < 4; ++i)
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = frame.scene,
                   .dstBinding = i,
                   .descriptorCount = 1,
                   .descriptorType =
                       i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                   .pBufferInfo = &buffers[i]};
    const VkDescriptorImageInfo images[] = {
        {shadowSampler, shadow->view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {sampler, resolved ? resolved->view : color->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler, depth->view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        reflections ? reflections->output()
                    : VkDescriptorImageInfo{sampler, resolved ? resolved->view : color->view,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    for (uint32_t i = 0; i < 4; ++i)
      writes[4 + i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                       .dstSet = i == 0 ? frame.scene : frame.composite,
                       .dstBinding = i == 0 ? 4u : i - 1,
                       .descriptorCount = 1,
                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       .pImageInfo = &images[i]};
    vkUpdateDescriptorSets(device, 8, writes, 0, nullptr);
    const auto irradiance = baking->descriptor();
    writes[8] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = frame.scene,
                 .dstBinding = 5,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 .pBufferInfo = &irradiance};
    vkUpdateDescriptorSets(device, 1, &writes[8], 0, nullptr);
    auto surfaceAtlas = baking->surfaceDescriptor();
    surfaceAtlas.sampler = surfaceSampler;
    writes[9] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = frame.scene,
                 .dstBinding = 6,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 .pImageInfo = &surfaceAtlas};
    vkUpdateDescriptorSets(device, 1, &writes[9], 0, nullptr);
    const VkDescriptorImageInfo localImages[] = {
        {shadowSampler, localMaps->staticView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {shadowSampler, localMaps->dynamicView(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    for (uint32_t i = 0; i < 2; ++i)
      writes[10 + i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = frame.scene,
                        .dstBinding = 7 + i,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        .pImageInfo = &localImages[i]};
    vkUpdateDescriptorSets(device, 2, &writes[10], 0, nullptr);
    auto occlusion = baking->occlusionDescriptor();
    occlusion.sampler = sampler;
    writes[12] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  .dstSet = frame.scene,
                  .dstBinding = 9,
                  .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                  .pImageInfo = &occlusion};
    vkUpdateDescriptorSets(device, 1, &writes[12], 0, nullptr);
    auto visibilityPages = baking->visibilityDescriptor();
    visibilityPages.sampler = sampler;
    writes[13] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                  .dstSet = frame.scene,
                  .dstBinding = 10,
                  .descriptorCount = 1,
                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                  .pImageInfo = &visibilityPages};
    vkUpdateDescriptorSets(device, 1, &writes[13], 0, nullptr);
    step(cpu.uploads);
    std::unordered_map<const scene::Material *, VkDescriptorSet> materials;
    for (auto *commands : {&frame.commands, &frame.staticCommands, &frame.terrainCommands, &frame.waterCommands})
      for (auto &draw : *commands) {
        const auto &material = draw.source.material;
        if (auto found = materials.find(material.get()); found != materials.end()) {
          draw.material = found->second;
          continue;
        }
        draw.material = allocate(frame, materialLayout);
        materials[material.get()] = draw.material;
        const auto fallback = resources.sampledMaterial(material, command);
        auto sample = [&](const std::shared_ptr<scene::Texture> &texture) {
          return texture ? resources.sampledTexture(material, texture, command) : fallback;
        };
        const VkDescriptorImageInfo textures[] = {
            fallback, sample(material->normalTexture), sample(material->roughnessTexture),
            sample(material->metallicTexture), sample(material->emissionTexture)};
        VkWriteDescriptorSet textureWrites[5];
        for (uint32_t i = 0; i < 5; ++i)
          textureWrites[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                              .dstSet = draw.material,
                              .dstBinding = i,
                              .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                              .pImageInfo = &textures[i]};
        vkUpdateDescriptorSets(device, 5, textureWrites, 0, nullptr);
      }
    step(cpu.materials);
    statistics.draws = uint32_t(frame.commands.size() + frame.terrainCommands.size() + frame.waterCommands.size());
    statistics.shadowedLights =
        uint32_t(std::count(shadowSelection.active.begin(), shadowSelection.active.end(), true));
    statistics.shadowOverflow = shadowSelection.overflow;
    statistics.staticShadowUpdates =
        uint32_t(std::count(shadowSelection.dirty.begin(), shadowSelection.dirty.end(), true));
    statistics.staticSunUpdates = uint32_t(std::count(frame.staticSunDirty.begin(), frame.staticSunDirty.end(), true));
    statistics.capturedCasters = uint32_t(std::count_if(frame.commands.begin(), frame.commands.end(),
        [](const auto &draw) { return draw.captured; }));
    statistics.liveCasters = uint32_t(frame.commands.size()) - statistics.capturedCasters;
    statistics.staticCasterPreparations = uint32_t(frame.staticCommands.size());
    probes::dumpFrameData(".shadow-preparation.bin", &statistics.staticCasterPreparations,
                          sizeof(statistics.staticCasterPreparations));
    const uint32_t shadowPolicy[] = {
        statistics.capturedCasters, statistics.liveCasters, statistics.staticSunUpdates,
        statistics.staticShadowUpdates,
        uint32_t(std::count_if(frame.commands.begin(), frame.commands.end(), [](const auto &draw) {
          return !draw.captured && draw.source.classification == REMIXAPI_INSTANCE_STATIC_EXT;
        }))};
    probes::dumpFrameData(".shadow-policy.bin", shadowPolicy, sizeof(shadowPolicy));
    statistics.lights = uint32_t(lights.size());
    statistics.clusters = data.grid[3];
    statistics.geometryBytes = 0;
    for (const auto &[key, value] : geometries)
      statistics.geometryBytes += value->vertices->allocationBytes + value->indices->allocationBytes;
    statistics.bufferBytes = statistics.geometryBytes;
    if (waterPass) statistics.bufferBytes += waterPass->bufferBytes();
    if (reflections)
      statistics.bufferBytes += reflections->bufferBytes();
    for (const auto &submitted : frames) {
      for (const auto *buffer : {submitted.constants.get(), submitted.lights.get(), submitted.draws.get(),
                                 submitted.clusters.get(), submitted.skyConstants.get()})
        if (buffer)
          statistics.bufferBytes += buffer->allocationBytes;
      for (const auto &geometry : submitted.posedGeometry)
        for (const auto *buffer : {geometry->vertices.get(), geometry->indices.get()})
          if (buffer) statistics.bufferBytes += buffer->allocationBytes;
    }
    step(cpu.publication);
    statistics.cpuRecording.preparation = cpu;
  }
} // namespace remix::forward
