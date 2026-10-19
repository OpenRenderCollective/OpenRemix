void finiteConeLightingTests(bool aggregate = false) {
  for (uint32_t samples : {1u, 2u, 4u}) {
    Target target(64, 64, true);
    auto &r = target.raster;
    remix::forward::Options options;
    options.samples = samples;
    options.debugView = 2; // Direct diffuse only; exclude indirect and specular.
    options.prism = false;
    options.ambient = 0;
    options.shadows = options.localShadows = options.ssr = false;
    options.shadowResolution = 64;
    options.baking.lightmapFitBudget = false;
    options.baking.samples = 2048;
    options.baking.bounces = 1;
    options.baking.maxProbes = 64;
    options.baking.reflectionProbes = 0;
    options.baking.lightmapPageSize = 64;
    options.baking.lightmapTexels = 4096;
    options.baking.diskCache = false;
    if (aggregate) {
      options.debugView = 0; // Shared irradiance plus its selected moving-shadow correction.
      options.baking.aggregateDiffuse = options.selectedBakedLights = options.localShadows = true;
      options.localShadowLights = 1;
      options.localShadowFade = 0;
    }
    target.enableForward(options);
    const auto mesh = quad(r, 98400, material(r, 98401, 1, 1, 1));
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 98402;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    auto receiver = instance(mesh);
    receiver.pNext = &identity;
    const auto retained = r.createRetained(receiver);
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {0, 0, -.3f};
    sphere.radius = .3f;
    sphere.shaping_hasvalue = 1;
    sphere.shaping_value.direction = {1, 0, 0};
    sphere.shaping_value.coneAngleDegrees = std::acos(.2f) * 180 / 3.14159265359f;
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = 98403;
    info.pNext = &sphere;
    info.radiance = {32, 0, 0};
    remixapi_LightHandle red;
    r.createLight(info, red);
    const float uv[] = {0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
    const remixapi_BakeSurfaceChartEXT chart{retained, 0, 0, 0, 6, 9, 9, uv};
    remixapi_BakeSceneSurfacesInfoEXT request{};
    auto &base = request.bounds.scene;
    base = {sizeof(request), REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT, 984, 1, 1,
            REMIXAPI_BAKE_SCENE_READY_EXT, 1, 1, &retained, &red};
    for (uint32_t c = 0; c < 3; ++c) {
      request.bounds.minimum[c] = 10;
      request.bounds.maximum[c] = 11;
    }
    request.chartRevision = request.chartCount = 1;
    request.charts = &chart;
    r.setBakeScene(&base);
    auto render = [&](bool charted) {
      camera(r);
      if (charted) r.drawRetained(nullptr);
      else r.drawInstance(instance(mesh));
      r.drawLight(red);
      return target.render();
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::vector<uint8_t> pixels;
    do {
      pixels = render(true);
      require(target.forward->statistics().baking.state != REMIXAPI_BAKE_FAILED_EXT,
              "Finite cone fixture bakes successfully");
      if (target.forward->statistics().baking.active) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    require(target.forward->statistics().baking.active, "Finite cone direct lighting publishes");
    const size_t centre = (32 * 64 + 32) * 4;
    const auto integrated = pixels[centre];
    if (aggregate) {
      require(integrated > 5 && target.forward->bakeController()->volume()->surfaceDirectOffset != 0,
              "Indexed aggregate retains finite-emitter energy outside the analytic centre cone");
      const auto black = quad(r, 98410, material(r, 98411, 0, 0, 0));
      auto moving = instance(black, -.4f);
      moving.doubleSided = 1;
      moving.transform.matrix[0][0] = moving.transform.matrix[1][1] = .5f;
      identity.instanceId = 98412;
      identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
      identity.rasterVisible = 0;
      moving.pNext = &identity;
      const auto actor = r.createRetained(moving);
      pixels = render(true);
      std::printf("Indexed aggregate cone %ux: clear %u, moving %u\n", samples,
                  unsigned(integrated), unsigned(pixels[centre]));
      require(pixels[centre] < integrated / 3,
              "Moving shadows still fetch captured corrections when the analytic cone response is zero");
      r.destroyRetained(&actor, 1);
      require(std::abs(int(render(true)[centre]) - int(integrated)) <= 1,
              "Removing the cone caster restores the clear aggregate without requiring a unit-field lookup");
      require(errors.load() == 0, "Indexed aggregate cone correction is validation clean");
      continue;
    }
    pixels = render(false);
    std::printf("Finite cone %ux: retained %u, analytic centre %u\n", samples,
                unsigned(integrated), unsigned(pixels[centre]));
    require(integrated > 5 && pixels[centre] < 3,
            "Finite emitter direct SH survives a zero centre-cone response");
    pixels = render(true);
    require(std::abs(int(pixels[centre]) - int(integrated)) <= 1,
            "Returning to a chart restores the same finite-cone direct contribution");
    require(errors.load() == 0, "Finite cone response remains Vulkan validation clean");
  }
}
