// Included beside the readback Target fixture in rendering.cpp.
std::array<uint8_t, 3> styledIndirectTests(bool directGroups = false, bool includeIndirect = false, uint32_t emitterCount = 1) {
  require(emitterCount > 0 && emitterCount <= 7, "Styled fixture fits the mixed-shadow slot budget");
  std::array<uint8_t, 3> litBySample{};
  uint32_t sampleOrdinal = 0;
  for (uint32_t samples : {1u, 2u, 4u}) {
    Target target(64, 64, true);
    auto &r = target.raster;
    remix::forward::Options options;
    options.samples = samples;
    options.ambient = 0;
    options.debugView = 1; // Indirect only: live direct/specular cannot satisfy this test.
    options.shadows = options.localShadows = options.ssr = false;
    options.baking.styledIndirect = !directGroups || includeIndirect;
    options.baking.groupedStyledDirect = directGroups;
    if (directGroups) {
      options.bakedLighting = true;
      options.bakedHighlightLights = 0;
      options.localShadowLights = 0;
    }
    options.baking.denoiseLightmaps = false; // Raw linear reconstruction is checked here.
    options.baking.styleStorageMiB = 1;
    options.baking.lightmapFitBudget = false;
    options.baking.lightmapPageSize = 32;
    options.baking.lightmapTexels = 1024;
    options.baking.lightmapLuxelSize = .125f;
    options.baking.maxProbes = 64;
    options.baking.spacing = .5f;
    options.baking.samples = 256;
    options.baking.bounces = 3;
    options.baking.batch = 16;
    options.baking.diskCache = false;
    options.baking.reflectionProbes = 0;
    target.enableForward(options);
    const auto mesh = quad(r, 18001, material(r, 18000, .8f, .8f, .8f));
    remixapi_InstanceIdentityEXT id{};
    id.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    id.instanceId = 18002;
    id.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    id.rasterVisible = 1;
    auto receiver = instance(mesh);
    receiver.pNext = &id;
    const auto surface = r.createRetained(receiver);
    auto ceiling = instance(mesh, -2);
    ceiling.pNext = &id;
    ceiling.doubleSided = 1;
    ceiling.transform.matrix[0][0] = 4;
    ceiling.transform.matrix[1][1] = -4;
    id.instanceId = 18003;
    id.rasterVisible = 0;
    const uint64_t instances[] = {surface, r.createRetained(ceiling)};
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {0, 0, -1};
    sphere.radius = .2f;
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &sphere;
    std::vector<remixapi_LightHandle> liveLights;
    std::vector<remixapi_BakeStyledLightEXT> styled;
    for (uint32_t i = 0; i < emitterCount; ++i) {
      light.hash = 18004 + i * 100;
      light.radiance = {32.f / emitterCount, 8.f / emitterCount, 2.f / emitterCount};
      remixapi_LightHandle authored, live;
      r.createLight(light, authored);
      light.hash = 18005 + i * 100;
      light.radiance = {};
      r.createLight(light, live);
      liveLights.push_back(live);
      styled.push_back({authored, live, 32, 0});
    }
    auto updateLive = [&] {
      for (uint32_t i = 0; i < emitterCount; ++i) {
        light.hash = 18005 + i * 100;
        r.updateLight(liveLights[i], light);
      }
    };
    remixapi_BakeSceneStyledLightsInfoEXT request{};
    auto &base = request.reflections.surfaces.bounds.scene;
    base = {sizeof(request), REMIXAPI_BAKE_SCENE_STYLED_LIGHTS_VERSION_EXT, 18006, 1, 1,
            REMIXAPI_BAKE_SCENE_READY_EXT, 2, 0, instances, nullptr};
    auto &bounds = request.reflections.surfaces.bounds;
    bounds.minimum[0] = bounds.minimum[1] = -.5f;
    bounds.maximum[0] = bounds.maximum[1] = .5f;
    bounds.minimum[2] = -1.5f;
    bounds.maximum[2] = .25f;
    const float uv[] = {0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
    const remixapi_BakeSurfaceChartEXT chart{surface, 0, 0, 0, 6, 9, 9, uv};
    request.reflections.surfaces.chartRevision = 1;
    request.reflections.surfaces.chartCount = 1;
    request.reflections.surfaces.charts = &chart;
    request.styledLightCount = uint32_t(styled.size());
    request.styledLights = styled.data();
    auto publish = [&] { r.setBakeScene(&base); };
    auto render = [&](bool charted) {
      camera(r);
      if (charted) r.drawRetained(nullptr);
      else r.drawInstance(instance(mesh)); // Same receiver, through dynamic probe reconstruction.
      for (auto live : liveLights) r.drawLight(live);
      return target.render();
    };
    auto until = [&](bool stylePhase = false) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      std::vector<uint8_t> image;
      do {
        image = render(true);
        const auto &stats = target.forward->statistics().baking;
        if (stats.state == REMIXAPI_BAKE_FAILED_EXT) std::fprintf(stderr, "%s\n", stats.status);
        require(stats.state != REMIXAPI_BAKE_FAILED_EXT, "Attributed map bake succeeds");
        const bool requested = !stylePhase ? bool(stats.active) : directGroups
            ? stats.state == REMIXAPI_BAKE_TRACING_EXT
            : std::strstr(stats.status, "Styled indirect") != nullptr;
        if (requested) return image;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
      require(false, "Attributed map bake reaches its requested phase");
      return image;
    };
    auto set = [&](float value) {
      light.radiance = {32 * value / emitterCount, 8 * value / emitterCount, 2 * value / emitterCount};
      updateLive();
      publish();
    };
    publish();
    until(true);
    r.setBakeScene(nullptr); // Cancellation with component submissions in flight.
    for (uint32_t i = 0; i < remix::gpuFrameCount + 2; ++i) render(true);
    require(!target.forward->statistics().baking.active, "Cancelled attributed work cannot publish");
    options.baking.diskCache = true;
    target.forward->updateLighting(options);
    publish();
    auto off = until();
    const size_t at = (32 * 64 + 32) * 4;
    const auto frozen = r.bakeScene();
    const auto volume = target.forward->bakeController()->volume();
    require(volume && volume->styledLighting && off[at] < 3,
            "Initially-off attributed map publishes style storage without residual indirect glow");
    if (directGroups)
      require(volume->groupedStyles == std::vector<uint32_t>{32},
              "Complete grouped direct publication retains its independent style identity");
    set(1);
    auto on = render(true);
    const auto lit = on[at];
    litBySample[sampleOrdinal++] = lit;
    if (directGroups)
      require(target.forward->statistics().analyticLights == 0 && target.forward->statistics().clusteredLights == 0,
              "Covered grouped direct receivers need no analytic light without highlights or moving corrections");
    require(lit > 8 && r.bakeScene() == frozen, "Surface indirect follows live style without rebaking");
    set(.5f);
    const auto partial = render(true);
    require(partial[at] > 3 && partial[at] < lit, "Surface indirect supports partial style values");
    set(0);
    require(render(true)[at] < 3, "Surface indirect switches fully off");
    set(1);
    const auto probeOn = render(false);
    set(0);
    const auto probeOff = render(false);
    if (directGroups) {
      light.radiance = {8.f / emitterCount, 32.f / emitterCount, 4.f / emitterCount};
      updateLive(); // Not one scalar multiple of the captured envelope.
      publish();
      require(render(true)[at] < 3 && target.forward->statistics().analyticLights == emitterCount &&
                  r.bakeScene() == frozen,
              "A non-scalar live colour disables grouped diffuse and restores analytic fallback without rebaking");
    }
    require(probeOn[at] > 8 && probeOff[at] < 3, "Dynamic receiver probes follow the same live style");
    set(1);
    r.setBakeScene(nullptr);
    for (uint32_t i = 0; i < remix::gpuFrameCount + 2; ++i) render(true);
    publish();
    const auto cached = until();
    require(std::strstr(target.forward->statistics().baking.status, "Cached") &&
            cached == on && target.forward->bakeController()->volume()->styledLighting,
            "Attributed cache reload preserves exact surface pixels and runtime style ownership");
    if (directGroups) {
      options.debugView = 0;
      options.localShadows = true;
      options.localShadowLights = emitterCount;
      options.localShadowFade = 0;
      target.enableForward(options, true);
      const auto clear = render(true);
      remixapi_InstanceIdentityEXT movingId{};
      movingId.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      movingId.instanceId = 18007;
      movingId.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
      movingId.rasterVisible = 0;
      auto blocker = instance(mesh, -.5f);
      blocker.pNext = &movingId;
      blocker.transform.matrix[0][0] = blocker.transform.matrix[1][1] = .4f;
      const auto moving = r.createRetained(blocker);
      const auto shadow = render(true);
      require(clear[at] > 8 && (includeIndirect ? shadow[at] > 3 && shadow[at] + 3 < clear[at]
                                               : shadow[at] < clear[at] / 3) &&
                  target.forward->statistics().staticShadowUpdates == 0,
              "A grouped controllable light subtracts only its moving-caster contribution from the lightmap");
      std::printf("Styled mixed %ux, %u emitters: clear %u, shadow %u, static updates %u\n",
                  samples, emitterCount, unsigned(clear[at]), unsigned(shadow[at]),
                  target.forward->statistics().staticShadowUpdates);
      r.destroyRetained(&moving, 1);
      require(std::abs(int(render(true)[at]) - int(clear[at])) <= 2,
              "Removing the moving caster restores the grouped direct contribution");
      set(0);
      require(render(true)[at] < 3, "The mixed grouped light still switches fully off");
    }
    std::printf("Styled %s %ux, %u emitters: surface %u/%u/%u, probe %u/%u; cache and cancellation passed\n",
                includeIndirect ? "combined" : directGroups ? "direct" : "indirect", samples, emitterCount, unsigned(off[at]), unsigned(partial[at]), unsigned(lit),
                unsigned(probeOff[at]), unsigned(probeOn[at]));
    require(errors.load() == 0, "Attributed map publication is Vulkan validation clean");
  }
  return litBySample;
}
