// A local light without a shadow map is occluded by a ray against the live
// scene when ray-traced local shadows are selected, and shines through
// geometry (the previous behaviour for every unmapped light) when they are not.
void rayLocalShadowTests() {
  for (uint32_t samples : {1u, 4u}) {
    Target target(64, 64, true);
    auto &r = target.raster;
    remix::forward::Options options;
    options.prism = false;
    options.samples = samples;
    options.ambient = 0;
    options.baking.enabled = options.shadows = options.ssr = false;
    options.baking.reflectionProbes = 0;
    options.splitCascades = true; // the receivers that carry the ray scene
    options.localShadows = true;
    options.localShadowLights = 0; // no map budget: every light is unmapped
    options.rtLocalShadows = true;
    target.enableForward(options);
    const auto wall = quad(r, 19101, material(r, 19100, .8f, .8f, .8f));
    const auto blockerMesh = quad(r, 19103, material(r, 19102, 0, 0, 0));
    remixapi_LightInfoSphereEXT sphere{};
    sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
    sphere.position = {0, 0, -.3f}; // in front of the wall's centre (the wall is at z = .5)
    sphere.radius = .1f;
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &sphere;
    light.hash = 19104;
    light.radiance = {30, 30, 30};
    remixapi_LightHandle handle;
    r.createLight(light, handle);
    // Halfway between the light and the wall, in the ray scene only: the camera
    // does not draw it, so the wall behind it stays visible.
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 19105;
    identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    identity.rasterVisible = 0;
    auto blocker = instance(blockerMesh, -.4f);
    blocker.doubleSided = 1;
    blocker.transform.matrix[0][0] = blocker.transform.matrix[1][1] = .25f;
    blocker.pNext = &identity;
    auto render = [&](bool blocked) {
      std::vector<uint8_t> image;
      // The ray scene follows the submitted draws; give a change a frame to reach it.
      for (int frame = 0; frame < 4; ++frame) {
        camera(r);
        r.drawLight(handle);
        r.drawInstance(instance(wall));
        if (blocked) r.drawInstance(blocker);
        image = target.render();
      }
      return image;
    };
    const size_t centre = (32 * 64 + 32) * 4, side = (32 * 64 + 10) * 4;
    auto image = render(false);
    const auto open = image[centre], openSide = image[side];
    image = render(true);
    std::printf("Ray local shadow %ux: open %u/%u, blocked %u/%u (%s)\n", samples, unsigned(open),
                unsigned(openSide), unsigned(image[centre]), unsigned(image[side]),
                target.forward->statistics().rayGI.status);
    require(target.forward->statistics().rayGI.active && open > 40 && open < 250 && openSide > 10,
            "The unmapped light reaches the open wall");
    require(image[centre] * 3 < open && image[side] + 6 > openSide,
            "A ray-traced shadow hides the unmapped light behind an offscreen blocker and nowhere else");
    image = render(false);
    require(image[centre] + 6 > open, "Removing the blocker restores the light");
    options.rtLocalShadows = false;
    target.enableForward(options);
    image = render(true);
    require(image[centre] + 6 > open,
            "Without ray-traced shadows an unmapped light is unshadowed, as before");
    // The sun, traced in place of its cascades. Sun shadows are switched off
    // and on live below, which separates the traced shadow from the cascades'.
    remixapi_LightInfoDistantEXT distant{};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = {0, 0, 1};
    remixapi_LightInfo sunInfo{};
    sunInfo.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    sunInfo.pNext = &distant;
    sunInfo.hash = 19106;
    sunInfo.radiance = {1, 1, 1};
    remixapi_LightHandle sun;
    r.createLight(sunInfo, sun);
    auto renderSun = [&](bool blocked) {
      std::vector<uint8_t> result;
      for (int frame = 0; frame < 4; ++frame) {
        camera(r);
        r.drawLight(sun);
        r.drawInstance(instance(wall));
        if (blocked) r.drawInstance(blocker);
        result = target.render();
      }
      return result;
    };
    options.shadows = true;
    options.shadowResolution = 256;
    options.rtSunShadows = true;
    target.enableForward(options);
    const auto sunlit = renderSun(false)[centre];
    image = renderSun(true);
    std::printf("Ray sun shadow %ux: open %u, blocked %u/%u\n", samples, unsigned(sunlit),
                unsigned(image[centre]), unsigned(image[side]));
    require(sunlit > 20 && image[centre] * 3 < sunlit && image[side] + 6 > sunlit,
            "A ray-traced sun shadow falls behind the caster and nowhere else");
    options.shadows = false;
    target.forward->updateLighting(options);
    require(renderSun(true)[centre] + 6 > sunlit, "Disabling sun shadows live stops tracing them");
    require(errors.load() == 0, "Ray-traced shadows are Vulkan validation clean");
  }
}
