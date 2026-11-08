void autoExposureTests() {
  // Raster HDR leaves sky samples uncovered. Meter the same visible dome that
  // the compositor later puts behind them, including fractional MSAA edges.
  for (uint32_t samples : {1u, 4u}) {
    Target target(64, 64);
    target.exposureSeconds = .1f;
    auto &r = target.raster;
    remix::forward::Options options;
    options.samples = samples;
    options.autoExposure = true;
    options.autoExposureTarget = .18f * .18f;
    options.ambient = .018f;
    options.shadows = options.localShadows = options.ssr = options.water = false;
    options.baking.enabled = false;
    options.baking.reflectionProbes = 1; // Meter the visible sky with inline reflections enabled.
    target.enableForward(options);
    remixapi_LightInfoDomeEXT dome{};
    dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    for (uint32_t c = 0; c < 3; ++c) dome.transform.matrix[c][c] = 1;
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &dome;
    light.hash = 889001;
    light.radiance = {.018f, .018f, .018f};
    remixapi_LightHandle sky;
    r.createLight(light, sky);
    for (uint32_t frame = 0; frame < 48; ++frame) {
      camera(r);
      r.drawLight(sky);
      target.render();
    }
    require(std::abs(target.forward->statistics().postProcessing.exposure - 10) < .01f,
            "A visible sky alone participates in automatic exposure");
    light.radiance = {18, 18, 18};
    r.createLight(light, sky);
    auto surface = instance(quad(r, 889003, material(r, 889002, 1, 1, 1)));
    surface.transform.matrix[0][0] = surface.transform.matrix[1][1] = .5f;
    for (uint32_t frame = 0; frame < 64; ++frame) {
      camera(r);
      r.drawLight(sky);
      r.drawInstance(surface);
      target.render();
    }
    require(target.forward->statistics().postProcessing.exposure < 1,
            "Bright visible sky prevents a dark raster receiver from overexposing the scene");
  }
  for (uint32_t samples : {1u, 4u}) {
    Target target(64, 64);
    target.exposureSeconds = .1f;
    auto &r = target.raster;
    remix::forward::Options options;
    options.samples = samples;
    options.autoExposure = true;
    options.autoExposureTarget = .18f * .18f;
    options.ambient = .18f;
    options.shadows = options.localShadows = options.ssr = options.water = false;
    options.ssr = true; // Exercise the additive reflection image and its GENERAL layout.
    options.ssrSteps = 16;
    options.baking.enabled = false;
    options.baking.reflectionProbes = 0;
    target.enableForward(options);
    auto mesh = quad(r, 88001, material(r, 88000, 1, 1, 1));
    const auto whiteMesh = mesh;
    auto render = [&] {
      camera(r);
      r.drawInstance(instance(mesh));
      return target.render();
    };
    auto settle = [&] {
      std::vector<uint8_t> image;
      for (uint32_t frame = 0; frame < 48; ++frame) image = render();
      return image;
    };
    const size_t centre = (32 * 64 + 32) * 4;
    const auto neutral = settle();
    const auto neutralExposure = target.forward->statistics().postProcessing.exposure;
    require(std::abs(neutralExposure - 1) < .01f, "Forward+ meters neutral material-normalized lighting");
    uint64_t colorHash = 88020;
    for (float albedo : {.25f, .75f}) {
      const auto surface = material(r, colorHash++, albedo, albedo, albedo);
      mesh = quad(r, colorHash++, surface);
      settle();
      require(std::abs(target.forward->statistics().postProcessing.exposure - 1) < .02f,
              "Material color changes do not masquerade as changes in illumination");
    }
    mesh = whiteMesh;
    settle();
    options.ambient = 1.8f;
    target.forward->updateLighting(options);
    const auto enteringBright = render();
    const auto bright = settle();
    require(enteringBright[centre] > neutral[centre] + 40 &&
                std::abs(int(bright[centre]) - neutral[centre]) <= 2,
            "Bright-room transition adapts smoothly back to the metered display value");
    options.ambient = .018f;
    target.forward->updateLighting(options);
    const auto enteringDark = render();
    const auto dark = settle();
    require(enteringDark[centre] + 20 < neutral[centre] &&
                std::abs(int(dark[centre]) - neutral[centre]) <= 2,
            "Dark-room transition brightens gradually without changing lighting assets");
    const auto state = target.forward->exposureState();
    options.samples = samples == 1 ? 4 : 1;
    target.enableForward(options, true);
    require(target.forward->exposureState() == state &&
                std::abs(int(render()[centre]) - neutral[centre]) <= 2,
            "Exposure survives MSAA target replacement without a brightness flash");
    options.debugView = 1;
    target.enableForward(options, true);
    options.ambient = 1.8f;
    target.forward->updateLighting(options);
    for (uint32_t frame = 0; frame < 8; ++frame) render();
    require(target.forward->statistics().postProcessing.meterFrames == 0 &&
                std::abs(target.forward->statistics().postProcessing.exposure - 10) < .01f,
            "Lighting debug views preserve exposure history while bypassing the meter");
    options.debugView = 0;
    options.ambient = .018f;
    target.enableForward(options, true);
    options.autoExposure = false;
    options.exposure = 10;
    target.enableForward(options, true);
    require(std::abs(int(render()[centre]) - neutral[centre]) <= 2,
            "Disabling automatic exposure restores the exact manual multiplier");
    options.autoExposure = true;
    options.exposure = 1;
    options.autoExposureMaxEv = 0;
    target.enableForward(options, true);
    settle();
    require(std::abs(target.forward->statistics().postProcessing.exposure - 1) < .001f &&
                target.forward->statistics().postProcessing.limit == 1,
            "EV limits bound dark-scene exposure");
    options.autoExposureMinEv = 0;
    options.ambient = 18;
    target.forward->updateLighting(options);
    settle();
    require(std::abs(target.forward->statistics().postProcessing.exposure - 1) < .001f &&
                target.forward->statistics().postProcessing.limit == -1,
            "Minimum EV limits bound bright-scene exposure");
    std::printf("Forward+ auto exposure %ux: display %u -> %u -> %u; target replacement/manual/EV passed\n",
                samples, neutral[centre], bright[centre], dark[centre]);
    options.autoExposure = false;
    options.ambient = .3f;
    options.prism = false;
    target.enableForward(options, true);
    require(std::abs(int(render()[centre]) - int(std::lround(.3f * 255))) <= 1,
            "Disabling Prism passes linear scene radiance without a hidden second tone curve");
    options.prism = true;
    const auto reference = prismDisplay({.3f, .3f, .3f});
    target.enableForward(options, true, true);
    require(std::abs(int(render()[centre]) - int(std::lround(std::pow(reference[0], 1.f / 2.2f) * 255))) <= 2,
            "Forward+ uses the path-traced Prism and gamma 2.2 display path exactly once");
    options.gamma22 = false;
    target.forward->updateLighting(options);
    const float srgb = 1.055f * std::pow(reference[0], 1.f / 2.4f) - .055f;
    require(std::abs(int(render()[centre]) - int(std::lround(srgb * 255))) <= 2,
            "Standard sRGB output remains available without double encoding");
  }
}
