void localShadowPlaneTests() {
  for (uint32_t samples : {1u, 4u}) {
    Target target(64, 64);
    auto &r = target.raster;
    remix::forward::Options options;
    options.samples = samples;
    options.ambient = 0;
    options.baking.enabled = options.shadows = options.ssr = false;
    options.localShadows = false;
    options.localShadowLights = 1;
    options.localShadowResolution = 128;
    options.localShadowFade = 0;
    target.enableForward(options);
    const auto mesh = quad(r, 19001, material(r, 19000, .8f, .8f, .8f));
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {-1.2f, 0, .45f}; // Light grazes an unobstructed planar wall.
    sphere.radius = .05f;
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &sphere;
    light.hash = 19002;
    light.radiance = {1000, 1000, 1000};
    remixapi_LightHandle handle;
    r.createLight(light, handle);
    auto render = [&] {
      camera(r);
      r.drawLight(handle);
      r.drawInstance(instance(mesh));
      return target.render();
    };
    const auto reference = render();
    options.localShadows = true;
    target.forward->updateLighting(options);
    const auto shadowed = render();
    uint32_t mismatched = 0, maximum = 0;
    for (uint32_t y = 16; y < 48; ++y) for (uint32_t x = 16; x < 48; ++x) {
      const size_t index = (y * 64 + x) * 4;
      const auto error = uint32_t(std::abs(int(reference[index]) - int(shadowed[index])));
      mismatched += error > 3;
      maximum = std::max(maximum, error);
    }
    std::printf("Unbaked point-light plane %ux: %u/1024 false-shadow pixels, maxerror %u\n",
                samples, mismatched, maximum);
    require(mismatched < 10, "An unobstructed wall must not self-shadow at grazing point-light angles");
    require(errors.load() == 0, "Point-light receiver-plane comparison is Vulkan validation clean");
  }
}
