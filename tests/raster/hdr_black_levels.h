// Exercise the actual Forward+ HDR compositor's low end. The UNORM readback
// can verify black/midtones, but cannot qualify above-paper-white HDR headroom.
void hdrBlackLevelTests() {
  for (uint32_t samples : {1u, 4u})
    for (bool encodeOutput : {false, true}) {
      Target target;
      auto &r = target.raster;
      remix::forward::Options options;
      options.samples = samples;
      options.ambient = 0;
      options.autoExposure = false;
      options.exposure = 1;
      options.baking.enabled = false;
      options.baking.reflectionProbes = 0;
      options.ssr = options.shadows = options.localShadows = false;
      options.prism = options.gamma22 = false; // HDR selects the shared display transform.
      target.enableForward(options, false, encodeOutput);
      target.forward->setDisplayOutput({true, 1000, 203});
      uint64_t hash = 0x4844520000;
      for (float value : {0.f, .0001f, .001f, .01f, .05f, .18f, .5f, 1.f}) {
        remixapi_MaterialInfoOpaqueEXT opaque{};
        opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
        opaque.opacityConstant = 1;
        opaque.alphaTestType = 7;
        remixapi_MaterialInfo info{};
        info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
        info.pNext = &opaque;
        info.hash = ++hash;
        info.emissiveIntensity = 1;
        info.emissiveColorConstant = {value, value, value};
        remixapi_MaterialHandle glow;
        r.createMaterial(info, glow);
        const auto mesh = quad(r, ++hash, glow);
        camera(r);
        r.drawInstance(instance(mesh));
        const auto pixels = target.render();
        const auto mapped = prismDisplay({value, value, value}, 1000.f / 203.f);
        const int expected = int(std::lround(std::pow(mapped[0], 1 / 2.2) * 255));
        pixel(pixels, 32, 32, expected, expected, expected, "HDR black/midtone shared display encoding");
        if (value == 0) {
          const auto centre = (32 * 64 + 32) * 4;
          require(pixels[centre] == 0 && pixels[centre + 1] == 0 && pixels[centre + 2] == 0,
                  "HDR compositor must not lift an unlit black surface");
        }
      }
    }
  require(errors.load() == 0, "HDR low-end compositor is validation clean");
}
