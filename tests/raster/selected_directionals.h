// Included inside rendering.cpp's fixture namespace.
void selectedDirectionalTests() {
  namespace f = remix::forward;
  namespace cache = f::bake::cache;
  for (uint32_t samples : {1u, 4u}) {
    Target target(64, 64, true);
    auto &r = target.raster;
    f::Options options;
    options.samples = samples;
    options.ambient = 0;
    options.prism = options.gamma22 = options.shadows = options.ssr = false;
    options.selectedBakedLights = true;
    options.localShadowLights = 7;
    options.localShadowResolution = 512;
    options.localShadowFade = 0;
    options.baking.aggregateDiffuse = true;
    options.baking.lightmapFitBudget = false;
    options.baking.spacing = .25f;
    options.baking.maxProbes = 2048;
    options.baking.samples = 2048;
    options.baking.bounces = 1;
    options.baking.batch = 32;
    options.baking.reflectionProbes = 0;
    options.baking.lightmapPageSize = 64;
    options.baking.lightmapTexels = 4096;
    target.enableForward(options);
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = opaque.roughnessConstant = 1;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo materialInfo{};
    materialInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    materialInfo.pNext = &opaque;
    materialInfo.hash = 0xd200;
    remixapi_MaterialHandle white;
    r.createMaterial(materialInfo, white);
    const auto mesh = quad(r, 0xd201, white);
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 0xd202;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    auto receiver = instance(mesh);
    receiver.pNext = &identity;
    const auto retained = r.createRetained(receiver);
    const auto black = quad(r, 0xd203, material(r, 0xd204, 0, 0, 0));
    std::vector<remixapi_LightHandle> lights;
    for (uint32_t i = 0; i < 7; ++i) {
      remixapi_LightInfoSphereEXT sphere{};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = {0, 0, -.8f};
      sphere.radius = .3f;
      remixapi_LightInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      info.pNext = &sphere;
      info.hash = 0xd210 + i;
      info.radiance = i == 6 ? remixapi_Float3D{0, 0, 8} : remixapi_Float3D{16, 0, 0};
      remixapi_LightHandle light;
      r.createLight(info, light);
      lights.push_back(light);
    }
    const float uv[] = {0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
    const remixapi_BakeSurfaceChartEXT chart{retained, 0, 0, 0, 6, 9, 9, uv};
    remixapi_BakeSceneSurfacesInfoEXT request{};
    auto &base = request.bounds.scene;
    base = {sizeof(request), REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT, 0xd220, 1, 1,
            REMIXAPI_BAKE_SCENE_READY_EXT, 1, uint32_t(lights.size()), &retained, lights.data()};
    request.bounds.minimum[0] = request.bounds.minimum[1] = -1;
    request.bounds.maximum[0] = request.bounds.maximum[1] = 1;
    request.bounds.minimum[2] = .25f;
    request.bounds.maximum[2] = .45f;
    request.chartRevision = 1;
    auto render = [&] {
      camera(r);
      r.drawRetained(nullptr);
      for (auto light : lights) r.drawLight(light);
      return target.render();
    };
    auto ready = [&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      do {
        auto pixels = render();
        const auto &stats = target.forward->statistics().baking;
        if (stats.state == REMIXAPI_BAKE_FAILED_EXT) std::fprintf(stderr, "%s\n", stats.status);
        require(stats.state != REMIXAPI_BAKE_FAILED_EXT, "Selected aggregate bake succeeds");
        if (stats.state == REMIXAPI_BAKE_READY_EXT) return render();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
      require(false, "Selected aggregate bake reaches publication");
      return std::vector<uint8_t>{};
    };
    const auto unload = [&] {
      r.setBakeScene(nullptr);
      for (uint32_t frame = 0; frame < remix::gpuFrameCount + 2; ++frame) render();
    };
    for (bool probes : {false, true}) {
      uint64_t dynamicReceiver = 0;
      if (probes) {
        identity.instanceId = 0xd202;
        identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
        identity.rasterVisible = 0;
        r.updateRetained(retained, receiver);
        identity.instanceId = 0xd230;
        identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
        identity.rasterVisible = 1;
        dynamicReceiver = r.createRetained(receiver);
      }
      request.chartCount = probes ? 0 : 1;
      request.charts = probes ? nullptr : &chart;
      ++request.chartRevision;
      ++base.revision;
      r.setBakeScene(&base);
      const auto lit = ready();
      const size_t centre = (32 * 64 + 32) * 4, adjacent = (24 * 64 + 32) * 4;
      require(lit[centre + 2] > 40, "Selected aggregate receiver has baked blue irradiance");
      auto moving = instance(black, -.4f);
      moving.doubleSided = 1;
      moving.transform.matrix[0][0] = .15f;
      moving.transform.matrix[1][1] = .04f;
      identity.instanceId = 0xd231;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT; // Settled, but absent from the authored bake.
      identity.rasterVisible = 0;
      moving.pNext = &identity;
      const auto actor = r.createRetained(moving);
      const auto shadow = render();
      std::printf("Selected %s %ux: blue lit %u, shadow %u, adjacent %u\n", probes ? "probe" : "surface",
          samples, lit[centre + 2], shadow[centre + 2], shadow[adjacent + 2]);
      require(shadow[centre + 2] < lit[centre + 2] / 3 && shadow[adjacent + 2] > lit[adjacent + 2] / 2,
              "Moving-object shadows subtract the extra baked light without darkening adjacent irradiance");
      require(target.forward->statistics().shadowedLights == 7 &&
                  target.forward->statistics().staticShadowUpdates == 0,
              "All seven selected mixed lights use moving-caster shadows without static world map updates");
      cache::persistence().waitIdle();
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(target.physical, &properties);
      cache::Device device{properties.vendorID, properties.deviceID, properties.driverVersion,
          properties.limits.maxImageDimension2D, properties.limits.maxImageArrayLayers,
          properties.limits.maxStorageBufferRange, {}};
      std::copy_n(properties.pipelineCacheUUID, device.uuid.size(), device.uuid.begin());
      const auto source = r.bakeScene();
      const auto policy = f::bake::staticAggregatePolicy(*source, f::bake::DirectMode::BakedMixed);
      const auto key = cache::key(source->contentHash, options.baking, options.lightCutoff, device, policy);
      const auto file = cache::path(key);
      auto cached = cache::read(file, key, source, options.baking, device, {}, policy);
      require(bool(cached), "Selected aggregate capture is persisted");
      const auto blue = uint32_t(std::find_if(source->lights.begin(), source->lights.end(),
          [](const auto &light) { return light.radiance[2] > 0; }) - source->lights.begin());
      const auto originalWords = cached->directionals;
      const auto originalProbes = cached->localProbes;
      if (probes) {
        require(cached->localProbes && cached->localProbes->directional(),
                "Dynamic receiver cache retains directional probe corrections");
        auto fields = std::vector(cached->localProbes->direct().begin(), cached->localProbes->direct().end());
        size_t changed = 0;
        for (size_t i = 0; i < fields.size(); ++i)
          if (cached->localProbes->entries()[i].light == blue) { fields[i] = {}; ++changed; }
        require(changed > 4, "Extra light owns directional fields across its complete probe volume");
        cached->localProbes = f::bake::ProbeVisibility::restore(uint32_t(cached->grid.probes.size()),
            uint32_t(source->lights.size()), cached->localProbes->offsets(), cached->localProbes->entries(),
            f::bake::ProbeVisibility::maximumBytes, fields);
      } else {
        require(cached->directionals.size() > 12 && cached->directionals[4] == 7 &&
                    std::find(cached->mixed.directCharts[0].begin(), cached->mixed.directCharts[0].end(), blue) ==
                        cached->mixed.directCharts[0].end(),
                "Surface correction targets a captured owner outside the four legacy channels");
        f::mixed::DirectionalFields replacement(1, uint32_t(source->lights.size()));
        const auto &rectangle = cached->layout.charts[0];
        for (uint32_t light = 0; light < source->lights.size(); ++light) {
          std::vector<f::mixed::directional::Field> values(rectangle.width * rectangle.height);
          for (uint32_t y = 0; y < rectangle.height; ++y)
            for (uint32_t x = 0; x < rectangle.width; ++x)
              if (light != blue)
                values[y * rectangle.width + x] = *f::mixed::DirectionalFields::sample(originalWords, 0, light, x, y);
          require(replacement.add(0, light, rectangle, values), "Controlled correction table fits");
        }
        cached->directionals = replacement.words();
      }
      const auto save = [&] {
        return cache::write(file, key,
            {cached->grid, cached->layout, cached->mixed, cached->captures, cached->specular, cached->pixels,
             cached->surfaceSamples, cached->directDiffuse, cached->sun, cached->styles,
             cached->localProbes ? &*cached->localProbes : nullptr,
             cached->directionalAttempted, cached->directionals});
      };
      require(save(), "Only the extra light's correction is blackened, preserving aggregate RGB");
      unload();
      r.setBakeScene(&base);
      const auto uncorrected = ready();
      require(std::strstr(target.forward->statistics().baking.status, "Cached lighting active") &&
                  uncorrected[centre + 2] > lit[centre + 2] * .7f,
              "Known-black extra-light correction leaves that light's aggregate energy intact under the caster");
      cached->directionals = originalWords;
      cached->localProbes = originalProbes;
      require(save(), "Restore the captured surface/probe unit responses");
      unload();
      r.setBakeScene(&base);
      const auto restored = ready();
      std::printf("Selected %s %ux correction: blue %u -> black-field %u -> restored %u\n",
          probes ? "probe" : "surface", samples, shadow[centre + 2], uncorrected[centre + 2], restored[centre + 2]);
      require(restored == shadow && std::strstr(target.forward->statistics().baking.status, "Cached lighting active"),
              "Cached extra-light mixed correction restores exact shadowed pixels");
      r.destroyRetained(&actor, 1);
      require(render() == lit, "Removing the caster restores the original aggregate receiver without trails");
      unload();
      if (dynamicReceiver) r.destroyRetained(&dynamicReceiver, 1);
    }
  }
}
