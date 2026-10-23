#include "forward/bake/styles.h"
#include "forward/gpu.h"
#include "forward/light_response.h"
#include "forward/material.h"
#include "forward/viewmodel/lighting.h"
#include "forward/probes/capture_dump.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace remix::forward {
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
    frame.probeVisibility.reset();
    frame.staticCommands.clear();
    frame.terrainCommands.clear();
    frame.waterCommands.clear();
    frame.viewModelCommands.clear();
    frame.viewModelCasters.clear();
    frame.viewModelScene = VK_NULL_HANDLE;
    frame.waterComposed = false;
    frame.waterMedium = false;
    auto sources = resources.sphereLights();
    std::sort(sources.begin(), sources.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    const auto publishedBake = baking->volume();
    std::vector<uint32_t> capturedLights(sources.size(), mixed::none);
    std::vector<uint8_t> fallbackShadows;
    if (publishedBake && publishedBake->mixedLights)
      for (uint32_t i = 0; i < sources.size(); ++i) {
        const auto &bindings = *publishedBake->mixedLights;
        const auto captured = bindings.match(sources[i]);
        capturedLights[i] = captured;
        // Live GI evaluates direct lights analytically, so those lights retain
        // ordinary shadow priority rather than becoming baked-only fallbacks.
        if (!giSupported && captured != mixed::none && bindings.bakedDiffuse(captured) &&
            !bindings.retainsVisibility(captured)) {
          if (fallbackShadows.empty()) fallbackShadows.resize(sources.size());
          fallbackShadows[i] = 1;
        }
      }
    const auto eye = transformPoint(scene::inverse(resources.view()), {0, 0, 0});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - epoch).count();
    shadowSelection = shadowPlanner.update(sources, eye, options.localShadows ? options.localShadowLights : 0,
                                           options.lightCutoff, options.localShadowDistance,
                                           options.sceneScale, resources.bakeScene(), seconds,
                                           options.localShadowFade, fallbackShadows);
    const auto bindings = publishedBake ? publishedBake->mixedLights : nullptr;
    if (highlightSource.lock() != bindings) {
      highlightSelection.reset();
      highlightSource = bindings;
    }
    std::vector<bake::StyleScale> liveStyles;
    std::array<uint8_t, 128> groupedStyles{};
    if (publishedBake && publishedBake->styledLighting && bindings) {
      liveStyles = bake::styleScales(*bindings->source, sources);
      for (const auto &scale : liveStyles)
        if (scale.status == bake::StyleStatus::Ready &&
            std::binary_search(publishedBake->groupedStyles.begin(), publishedBake->groupedStyles.end(), scale.style))
          groupedStyles[scale.style] = 1;
    }
    const auto grouped = [&](uint32_t captured) {
      if (giSupported || !bindings || captured >= bindings->source->lights.size()) return false;
      const auto style = bindings->source->lights[captured].style;
      return style < groupedStyles.size() && groupedStyles[style] != 0;
    };
    std::vector<float> highlightWeights;
    std::vector<HighlightCandidate> highlightCandidates;
    if (options.bakedLightingActive(giSupported) && bindings) {
      // Captured indices distinguish emitters even when the adapter shares IDs.
      // Merge repeated submissions of the same captured emitter before selection.
      std::vector<float> priorities(bindings->source->lights.size(), 0);
      for (uint32_t i = 0; i < sources.size(); ++i) {
        const auto captured = capturedLights[i];
        if (captured == mixed::none || (!bindings->bakedDiffuse(captured) && !grouped(captured)) ||
            !bindings->retainsVisibility(captured) || bindings->source->lights[captured].bakeHint ==
                REMIXAPI_LIGHT_BAKING_REQUIRE_MIXED_EXT) continue;
        const auto &source = sources[i];
        const float range = influenceRadius(source, options.lightCutoff);
        if (!std::isfinite(range) || range <= 0) continue;
        Vec3 delta;
        for (uint32_t c = 0; c < 3; ++c) delta[c] = eye[c] - source.position[c];
        const float priority = range * range / std::max(dot(delta, delta), 1e-6f);
        priorities[captured] = std::max(priorities[captured], priority);
      }
      highlightWeights.resize(priorities.size(), -1);
      for (uint32_t i = 0; i < priorities.size(); ++i)
        if (priorities[i] > 0) highlightCandidates.push_back({i, priorities[i]});
    }
    for (const auto &highlight : highlightSelection.update(highlightCandidates,
             options.bakedHighlightLights, seconds, options.bakedHighlightFade))
      highlightWeights[highlight.key] = highlight.value;
    step(cpu.selection);
    auto &draws = drawScratch;
    draws.clear();
    draws.reserve(resources.draws().size());
    std::vector<water::MaterialData> waterMaterials;
    Bounds casters;
    auto append = [&](const scene::Draw &draw, std::vector<Command> &commands, bool staticOnly, bool liquid = false) {
      if (!(liquid ? water::accepts(draw) : accepts(draw) ||
            (!staticOnly && (acceptsSurfaceLayer(draw) || acceptsViewModel(draw)))))
        return;
      const uint32_t count = draw.indexCount ? draw.indexCount : uint32_t(draw.surface->indices.size());
      if (!count)
        return;
      Command out;
      out.geometry = geometry(draw);
      out.source = draw;
      out.captured = !liquid && !draw.viewModel && (staticOnly || shadowPlanner.captured(draw));
      if (!staticOnly && !liquid && !draw.viewModel)
        if (const auto &volume = baking->volume(); volume && volume->surfaces)
          out.lightmap = volume->surfaces->find(draw);
      out.index = uint32_t(draws.size());
      out.count = count;
      out.worldBounds = transformBounds(out.geometry->bounds, draw.model);
      if (!liquid && !draw.viewModel) casters.merge(out.worldBounds);
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
                   : draw.viewModel ? frame.viewModelCommands
                   : acceptsSurfaceLayer(draw) ? frame.terrainCommands : frame.commands, false, liquid);
    }
    if (!frame.viewModelCommands.empty()) viewModelTarget->ensure();
    const uint32_t capturedCasters = uint32_t(std::count_if(frame.commands.begin(), frame.commands.end(),
        [](const auto &draw) { return draw.captured; }));
    auto emptyMovingShadows = shadowSelection.active;
    // The common frozen-only scene needs no additional caster traversal. With
    // actors present, use the same conservative light-volume test as drawing.
    if (capturedCasters != frame.commands.size())
      for (uint32_t lightSlot = 0; lightSlot < shadows::maximumLights; ++lightSlot)
        if (shadowSelection.active[lightSlot])
          emptyMovingShadows[lightSlot] = std::none_of(frame.commands.begin(), frame.commands.end(),
              [&](const Command &draw) { return castsLocalShadow(draw, lightSlot, false); });
    // Complete frozen bounds still fit the cascades even when no static draw
    // descriptors or material records need preparing for this frame.
    if (options.shadows && sun.isDistant)
      casters.merge(frozenShadowBounds());
    step(cpu.draws);
    auto &data = frame.data;
    data = {};
    if (const auto &volume = baking->volume(); volume && volume->styledLighting && volume->mixedLights) {
      for (const auto &scale : liveStyles)
        if (scale.status == bake::StyleStatus::Ready)
          data.styleScales[scale.style / 4][scale.style % 4] = scale.value;
    }
    if (baking->volume()) {
      data.occlusion = baking->volume()->occlusionParameters;
      data.clusterInfo[2] = baking->volume()->surfaceDirectOffset;
    }
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
    static const bool selectiveDirect = [] {
      const auto value = std::getenv("REMIX_DEBUG_DIRECT_FETCH");
      return !value || std::strcmp(value, "0") != 0;
    }();
    const bool selectDirectOwners = selectiveDirect && bindings && !giSupported &&
        std::any_of(bindings->directLighting.lights.begin(), bindings->directLighting.lights.end(),
                    bake::aggregatesDiffuse);
    std::vector<uint8_t> unitDirectNeeded(selectDirectOwners ? bindings->source->lights.size() : 0);
    statistics.analyticLights = statistics.aggregateLights = statistics.selectedAggregateLights = 0;
    statistics.requiredMixedLights = 0;
    statistics.selectedHighlightLights = statistics.selectedMixedShadowLights = 0;
    const float clusterRadiusScale = viewRadiusScale(data.view);
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
      light.clusterBounds = clusterSphereBounds(data.view,
          {source.position[0], source.position[1], source.position[2]}, radius, clusterRadiusScale);
      const auto shadowSlot = shadowSelection.slots[sourceIndex];
      LightResponseInput responseInput;
      responseInput.selectedOnly = options.selectBakedLights(giSupported);
      responseInput.shadowSelected = shadowSlot != UINT32_MAX;
      responseInput.shadowWeight = responseInput.shadowSelected ? shadowSelection.strength[shadowSlot] : 0;
      responseInput.independentHighlights = options.bakedLightingActive(giSupported);
      const auto captured = capturedLights[sourceIndex];
      if (captured < highlightWeights.size()) responseInput.highlightWeight = highlightWeights[captured];
      if (publishedBake && publishedBake->mixedLights) {
        if (captured != mixed::none) {
          light.shaping[3] = float(captured + 1);
          const auto &bindings = *publishedBake->mixedLights;
          responseInput.aggregate = bindings.bakedDiffuse(captured) || grouped(captured);
          responseInput.attributed = bindings.retainsVisibility(captured);
          responseInput.requireMixed = bindings.source->lights[captured].bakeHint ==
                                    REMIXAPI_LIGHT_BAKING_REQUIRE_MIXED_EXT;
        }
      }
      const auto response = lightResponse(responseInput);
      switch (response.mode) {
      case LightResponse::Analytic: ++statistics.analyticLights; break;
      case LightResponse::Aggregate: ++statistics.aggregateLights; break;
      case LightResponse::SelectedAggregate:
        ++statistics.selectedAggregateLights;
        statistics.selectedHighlightLights += response.highlightWeight > 0;
        statistics.selectedMixedShadowLights += responseInput.shadowSelected && responseInput.shadowWeight > 0;
        break;
      case LightResponse::BakedFallback: break;
      }
      if (response.mode != LightResponse::BakedFallback && responseInput.requireMixed)
        ++statistics.requiredMixedLights;
      if (captured < unitDirectNeeded.size())
        unitDirectNeeded[captured] |= needsUnitDirect(
            response.mode, responseInput.shadowSelected && !emptyMovingShadows[shadowSlot], responseInput.shadowWeight);
      light.corners[2][3] = float(response.mode);
      light.responseWeights[0] = response.highlightWeight;
      for (int i = 0; i < 3; ++i)
        std::copy_n(source.corners[i], 3, light.corners[i].begin());
      if (const auto slot = shadowSelection.slots[sourceIndex]; slot != UINT32_MAX) {
        light.corners[0][3] = float(slot + 1);
        light.corners[1][3] = 1 - shadowSelection.strength[slot];
        light.responseWeights[1] = emptyMovingShadows[slot] ? 1.f : 0.f;
      }
      lights.push_back(light);
    }
    if (selectDirectOwners) {
      for (const auto *commands : {&frame.commands, &frame.terrainCommands})
        for (const auto &draw : *commands) {
          if (!draw.lightmap) continue;
          const auto &owners = draw.lightmap->directOwners;
          if (!drawNeedsUnitDirect(owners, unitDirectNeeded)) draws[draw.index].maps[0] |= skipUnitDirect;
        }
    }
    const auto firstBaked = std::find_if(lights.begin(), lights.end(),
        [](const auto &light) { return light.corners[2][3] == float(LightResponse::BakedFallback); });
    const auto mixedEnd = std::stable_partition(firstBaked, lights.end(),
        [](const auto &light) { return light.corners[2][3] != float(LightResponse::BakedFallback); });
    const uint32_t runtimeLights = uint32_t(mixedEnd - lights.begin());
    // Zero preserves the ordinary full-list contract. Otherwise the prefix
    // contains mixed/dynamic lights; the baked tail is only an uncovered fallback.
    data.clusterInfo[1] = firstBaked == lights.end() ? 0 : runtimeLights + 1;
    static const bool clusterFallback = std::getenv("REMIX_DEBUG_FORWARD_UNCLUSTERED_FALLBACK") == nullptr;
    if (clusterFallback && firstBaked != lights.end()) data.clusterInfo[1] |= clusteredBakedTail;
    data.target = {extent.width, extent.height, uint32_t(lights.size()),
                   uint32_t(options.shadows && sun.isDistant)};
    frame.staticSunDirty = sunCache.update(shadowPlanner.source(), data.target[3] != 0, data.shadows, options.shadowResolution);
    const bool refreshLocal = std::any_of(shadowSelection.dirty.begin(), shadowSelection.dirty.end(),
                                         [](bool dirty) { return dirty; });
    const bool refreshSun = data.target[3] && std::any_of(frame.staticSunDirty.begin(), frame.staticSunDirty.end(),
                                                         [](bool dirty) { return dirty; });
    if ((refreshLocal || refreshSun) && shadowPlanner.source())
      for (const auto &draw : shadowPlanner.source()->draws)
        append(draw, frame.staticCommands, true);
    if (!frame.viewModelCommands.empty() && data.target[3]) {
      if (!viewModelSun) {
        viewModelSun = std::make_unique<viewmodel::SunShadow>();
        viewModelSun->initialize(physical, device, shadowPass, options.shadowResolution);
      }
      Bounds receivers;
      for (const auto &draw : frame.viewModelCommands) receivers.merge(draw.worldBounds);
      Bounds allCasters = casters;
      allCasters.merge(receivers);
      frame.viewModelProjection = viewmodel::sunProjection(receivers, allCasters, direction,
                                                           options.shadowResolution);
      if (shadowPlanner.source())
        for (const auto &draw : shadowPlanner.source()->draws) {
          if (!accepts(draw)) continue;
          const auto bounds = transformBounds(geometry(draw)->bounds, draw.model);
          if (intersectsClip(bounds, frame.viewModelProjection)) append(draw, frame.viewModelCasters, true);
        }
    }
    step(cpu.lighting);
    if (probeVisibility && !giSupported) {
      std::vector<uint32_t> selected;
      for (uint32_t i = 0; i < runtimeLights; ++i) {
        auto &light = lights[i];
        const auto captured = uint32_t(light.shaping[3]);
        if (captured == 0 || light.corners[2][3] <= .5f) continue;
        auto found = std::find(selected.begin(), selected.end(), captured);
        if (found == selected.end()) {
          if (selected.size() == probe_batch::Cache::maximumLights) continue;
          selected.push_back(captured);
          found = selected.end() - 1;
        }
        light.responseWeights[2] = float(found - selected.begin() + 1);
      }
      frame.probeVisibility = probeVisibility->record(command, publishedBake, selected);
      if (!frame.probeVisibility)
        for (auto &light : lights) light.responseWeights[2] = 0;
    }
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    data.clusterInfo[0] = clusterListCapacity(runtimeLights, data.grid[3],
        properties.limits.maxStorageBufferRange, options.clusterLights);
    statistics.clusterLightCapacity = data.clusterInfo[0];
    const VkDeviceSize clusterBytes =
        VkDeviceSize(data.grid[3]) * (data.clusterInfo[0] + clusterHeaderWords) * sizeof(uint32_t);
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
                                      frame.terrainCommands.size() + frame.waterCommands.size() +
                                      frame.viewModelCommands.size() + frame.viewModelCasters.size()) + 3;
    if (!frame.pool || frame.poolCapacity < capacity) {
      if (frame.pool)
        vkDestroyDescriptorPool(device, frame.pool, nullptr);
      frame.pool = VK_NULL_HANDLE;
      frame.poolCapacity = std::max(64u, capacity);
      const VkDescriptorPoolSize sizes[] = {
          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 7},
          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, frame.poolCapacity * 5 + 21}};
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
                          {reflectionDescriptor(), frame.skyConstants->descriptor(), frame.skyImage,
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
    const VkDescriptorImageInfo viewModelImage{sampler,
        frame.viewModelCommands.empty() ? images[1].imageView : viewModelTarget->color(),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet viewModelWrite{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = frame.composite, .dstBinding = 6, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &viewModelImage};
    vkUpdateDescriptorSets(device, 1, &viewModelWrite, 0, nullptr);
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
    // The world shader does not read this binding. Keep it valid so an overlay
    // descriptor copy can replace it with the isolated self-shadow array.
    const VkWriteDescriptorSet selfShadowDefault{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = frame.scene, .dstBinding = 11, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &localImages[1]};
    vkUpdateDescriptorSets(device, 1, &selfShadowDefault, 0, nullptr);
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
    const VkDescriptorBufferInfo sunDepthBuffers[]{baking->sunDepthTable(), baking->sunDepthValues()};
    VkWriteDescriptorSet sunDepthWrites[2];
    for (uint32_t i = 0; i < 2; ++i)
      sunDepthWrites[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = frame.scene,
          .dstBinding = 20 + i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &sunDepthBuffers[i]};
    vkUpdateDescriptorSets(device, 2, sunDepthWrites, 0, nullptr);
    // A declined optional field keeps every selected slot zero. Bind a valid
    // existing buffer as fallback; the shader never addresses it in that case.
    const auto probeField = frame.probeVisibility ? frame.probeVisibility->descriptor() : irradiance;
    const VkWriteDescriptorSet probeWrite{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = frame.scene, .dstBinding = 22, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &probeField};
    vkUpdateDescriptorSets(device, 1, &probeWrite, 0, nullptr);
    prepareViewModelDescriptors();
    step(cpu.uploads);
    std::unordered_map<const scene::Material *, VkDescriptorSet> materials;
    for (auto *commands : {&frame.commands, &frame.staticCommands, &frame.terrainCommands,
                           &frame.waterCommands, &frame.viewModelCommands, &frame.viewModelCasters})
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
    statistics.draws = uint32_t(frame.commands.size() + frame.terrainCommands.size() +
                                frame.waterCommands.size() + frame.viewModelCommands.size());
    statistics.shadowedLights =
        uint32_t(std::count(shadowSelection.active.begin(), shadowSelection.active.end(), true));
    statistics.emptyMovingShadowMaps =
        uint32_t(std::count(emptyMovingShadows.begin(), emptyMovingShadows.end(), true));
    statistics.shadowOverflow = shadowSelection.overflow;
    statistics.staticShadowUpdates =
        uint32_t(std::count(shadowSelection.dirty.begin(), shadowSelection.dirty.end(), true));
    statistics.staticSunUpdates = uint32_t(std::count(frame.staticSunDirty.begin(), frame.staticSunDirty.end(), true));
    statistics.capturedCasters = capturedCasters;
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
    statistics.clusteredLights = runtimeLights;
    statistics.clusterBuildLights =
        (data.clusterInfo[1] & clusteredBakedTail) ? uint32_t(lights.size()) : runtimeLights;
    statistics.fullyBakedLights = uint32_t(lights.size()) - runtimeLights;
    statistics.clusters = data.grid[3];
    statistics.geometryBytes = spareGeometryBytes;
    statistics.spareGeometryBytes = spareGeometryBytes;
    for (const auto &[key, value] : geometries)
      statistics.geometryBytes += value->vertices->allocationBytes + value->indices->allocationBytes;
    statistics.bufferBytes = statistics.geometryBytes;
    if (waterPass) statistics.bufferBytes += waterPass->bufferBytes();
    if (reflections)
      statistics.bufferBytes += reflections->bufferBytes();
    for (const auto &submitted : frames) {
      for (const auto *buffer : {submitted.constants.get(), submitted.lights.get(), submitted.draws.get(),
                                 submitted.clusters.get(), submitted.skyConstants.get(),
                                 submitted.viewModelConstants.get(), submitted.viewModelReflectionConstants.get()})
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
