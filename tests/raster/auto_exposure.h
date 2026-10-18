void autoExposureTests() {
  for (uint32_t samples : {1u, 4u}) {
    Target target(64, 64);
    target.exposureSeconds = .1f;
    auto &r = target.raster;
    remix::forward::Options options;
    options.samples = samples;
    options.autoExposure = true;
    options.ambient = .18f;
    options.shadows = options.localShadows = options.ssr = options.water = false;
    options.ssr = true; // Exercise the additive reflection image and its GENERAL layout.
    options.ssrSteps = 16;
    options.baking.enabled = false;
    options.baking.reflectionProbes = 0;
    target.enableForward(options);
    const auto mesh = quad(r, 88001, material(r, 88000, 1, 1, 1));
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
    require(std::abs(neutralExposure - 1) < .01f, "Forward+ meters neutral world radiance");
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
    target.forward->updateLighting(options);
    require(std::abs(int(render()[centre]) - neutral[centre]) <= 2,
            "Disabling automatic exposure restores the exact manual multiplier");
    options.autoExposure = true;
    options.exposure = 1;
    options.autoExposureMaxEv = 0;
    target.forward->updateLighting(options);
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
  }
}
