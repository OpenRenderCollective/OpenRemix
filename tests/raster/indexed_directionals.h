// Included inside rendering.cpp's fixture namespace.
void indexedDirectionalTests() {
  namespace f = remix::forward;
  namespace cache = f::bake::cache;
  for (uint32_t samples : {1u, 2u, 4u}) {
    Target target(64, 64, true);
    auto &r = target.raster;
    f::Options options;
    options.samples = samples;
    options.ambient = 0;
    options.debugView = 2; // Isolate direct diffuse from specular and shared indirect energy.
    options.prism = options.gamma22 = false;
    options.shadows = options.localShadows = options.ssr = false;
    options.baking.lightmapFitBudget = false;
    options.baking.maxProbes = 64;
    options.baking.spacing = 2;
    options.baking.samples = 64;
    options.baking.bounces = 1;
    options.baking.batch = 8;
    options.baking.reflectionProbes = 0;
    options.baking.lightmapPageSize = 64;
    options.baking.lightmapTexels = 4096;
    options.baking.diskCache = false;
    target.enableForward(options);
    const auto mesh = quad(r, 0xd100, material(r, 0xd101, 1, 1, 1));
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 0xd102;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    auto receiver = instance(mesh);
    receiver.pNext = &identity;
    const auto retained = r.createRetained(receiver);
    std::vector<remixapi_LightHandle> lights;
    for (uint32_t i = 0; i < 7; ++i) {
      remixapi_LightInfoSphereEXT sphere{};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.position = {0, 0, -.8f};
      sphere.radius = .3f;
      remixapi_LightInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      info.pNext = &sphere;
      info.hash = 0xd110 + i;
      info.radiance = i == 6 ? remixapi_Float3D{0, 0, 8} : remixapi_Float3D{16, 0, 0};
      remixapi_LightHandle light;
      r.createLight(info, light);
      lights.push_back(light);
    }
    const float uv[] = {0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
    const remixapi_BakeSurfaceChartEXT chart{retained, 0, 0, 0, 6, 9, 9, uv};
    remixapi_BakeSceneSurfacesInfoEXT request{};
    auto &base = request.bounds.scene;
    base = {sizeof(request), REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT, 0xd120, 1, 1,
            REMIXAPI_BAKE_SCENE_READY_EXT, 1, uint32_t(lights.size()), &retained, lights.data()};
    std::fill_n(request.bounds.minimum, 3, 10.f);
    std::fill_n(request.bounds.maximum, 3, 11.f);
    request.chartRevision = request.chartCount = 1;
    request.charts = &chart;
    auto render = [&] {
      camera(r);
      r.drawRetained(nullptr);
      for (auto light : lights) r.drawLight(light);
      return target.render();
    };
    auto until = [&](bool ready) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
      std::vector<uint8_t> pixels;
      do {
        pixels = render();
        const auto &stats = target.forward->statistics().baking;
        if (stats.state == REMIXAPI_BAKE_FAILED_EXT) std::fprintf(stderr, "%s\n", stats.status);
        require(stats.state != REMIXAPI_BAKE_FAILED_EXT, "Seven-owner directional bake succeeds");
        if (ready ? stats.state == REMIXAPI_BAKE_READY_EXT
                  : std::strstr(stats.status, "Baking attributed light") != nullptr)
          return pixels;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      } while (std::chrono::steady_clock::now() < deadline);
      require(false, "Seven-owner bake reaches its requested controller phase");
      return pixels;
    };
    const auto unload = [&] {
      r.setBakeScene(nullptr);
      for (uint32_t frame = 0; frame < remix::gpuFrameCount + 2; ++frame) render();
    };
    r.setBakeScene(&base);
    until(false);
    unload();
    require(!target.forward->statistics().baking.active,
            "Cancelling an extra directional field cannot publish a stale volume");
    options.baking.diskCache = true;
    target.forward->updateLighting(options);
    ++base.revision;
    r.setBakeScene(&base);
    const auto original = until(true);
    const size_t centre = (32 * 64 + 32) * 4;
    require(original[centre] > 20 && original[centre + 2] > 80 &&
                target.forward->bakeController()->volume()->surfaceDirectOffset != 0,
            "Seven-owner controller capture publishes indexed directional shading on the GPU");
    cache::persistence().waitIdle();
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(target.physical, &properties);
    cache::Device device{properties.vendorID, properties.deviceID, properties.driverVersion,
        properties.limits.maxImageDimension2D, properties.limits.maxImageArrayLayers,
        properties.limits.maxStorageBufferRange, {}};
    std::copy_n(properties.pipelineCacheUUID, device.uuid.size(), device.uuid.begin());
    const auto source = r.bakeScene();
    const auto key = cache::key(source->contentHash, options.baking, options.lightCutoff, device);
    const auto file = cache::path(key);
    auto cached = cache::read(file, key, source, options.baking, device);
    require(cached && cached->directionalAttempted && cached->directionals.size() > 12 &&
                cached->directionals[4] == 7,
            "Controller persistence contains all seven captured directional owners");
    const auto originalWords = cached->directionals;
    const auto blue = uint32_t(std::find_if(source->lights.begin(), source->lights.end(),
        [](const auto &light) { return light.radiance[2] > 0; }) - source->lights.begin());
    require(blue < source->lights.size() &&
                std::find(cached->mixed.directCharts[0].begin(), cached->mixed.directCharts[0].end(), blue) ==
                    cached->mixed.directCharts[0].end(),
            "Decoder isolation targets an owner outside all four legacy directional channels");
    f::mixed::DirectionalFields blackBlue(1, uint32_t(source->lights.size()));
    const auto &rectangle = cached->layout.charts[0];
    for (uint32_t light = 0; light < source->lights.size(); ++light) {
      std::vector<f::mixed::directional::Field> values(rectangle.width * rectangle.height);
      for (uint32_t y = 0; y < rectangle.height; ++y)
        for (uint32_t x = 0; x < rectangle.width; ++x)
          if (light != blue) {
            const auto value = f::mixed::DirectionalFields::sample(originalWords, 0, light, x, y);
            require(bool(value), "Every positive owner has a complete rectangle");
            values[y * rectangle.width + x] = *value;
          }
      require(blackBlue.add(0, light, rectangle, values), "Controlled seventh-owner field fits");
    }
    const auto save = [&] {
      return cache::write(file, key,
          {cached->grid, cached->layout, cached->mixed, cached->captures, cached->specular, cached->pixels,
           cached->surfaceSamples, cached->directDiffuse, cached->sun, cached->styles,
           cached->localProbes ? &*cached->localProbes : nullptr, true, cached->directionals});
    };
    cached->directionals = blackBlue.words();
    require(save(), "Controlled known-black seventh owner replaces only its cached unit response");
    unload();
    r.setBakeScene(&base);
    const auto black = until(true);
    require(std::strstr(target.forward->statistics().baking.status, "Cached lighting active") &&
                black[centre + 2] < 4 && std::abs(int(black[centre]) - int(original[centre])) < 3,
            "GLSL reads the seventh owner's known-black field instead of falling back to analytic diffuse");
    cached->directionals = originalWords;
    require(save(), "Restore captured directional coefficients after decoder isolation");
    unload();
    r.setBakeScene(&base);
    const auto restored = until(true);
    require(std::strstr(target.forward->statistics().baking.status, "Cached lighting active") &&
                restored == original,
            "Seven-owner disk reload restores exact captured GPU shading without transient receiver arrays");
    std::printf("Indexed seven-owner %ux MSAA: blue %u -> %u -> %u\n", samples,
        original[centre + 2], black[centre + 2], restored[centre + 2]);
  }
}
