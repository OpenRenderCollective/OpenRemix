// Included after the shared perspective-camera helper in rendering.cpp.
void composedViewmodelSunTests(bool cascades = false) {
  namespace f = remix::forward;
  for (uint32_t samples : {1u, 2u, 4u}) {
    Target target(64, 64, true);
    auto &r = target.raster;
    f::Options options;
    options.samples = samples;
    options.rtGI = !cascades;
    options.radianceCascades = cascades;
    options.bakedDirectWithLiveGI = options.bakedLighting = true;
    options.cascadeCount = 2;
    options.cascadeDirections = 4;
    options.cascadeHistory = 0;
    options.cascadeBounces = false;
    options.rtGISamples = options.rtGIBounces = 1;
    options.rtGIDistance = .1f; // isolate direct sun; incoming transport has separate fixtures
    options.prism = options.gamma22 = options.ssr = options.localShadows = false;
    options.ambient = 0;
    options.shadowResolution = 512;
    options.shadowDistance = 20;
    options.baking.spacing = .125f;
    options.baking.maxProbes = 8192;
    options.baking.samples = options.baking.batch = 64;
    options.baking.bounces = 1;
    options.baking.reflectionProbes = 0;
    options.baking.diskCache = options.baking.denoiseLightmaps = false;
    target.enableForward(options);

    const auto white = material(r, 0xe100, 1, 1, 1);
    const auto black = material(r, 0xe101, 0, 0, 0);
    const auto mesh = quad(r, 0xe102, white);
    const auto blackMesh = quad(r, 0xe103, black);
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    identity.instanceId = 0xe104;
    auto wall = instance(mesh, 7.5f);
    wall.doubleSided = 1;
    wall.transform.matrix[0][0] = wall.transform.matrix[1][1] = 20;
    wall.pNext = &identity;
    const auto wallHandle = r.createRetained(wall);
    identity.instanceId = 0xe105;
    identity.rasterVisible = 0;
    auto frozen = instance(blackMesh, .5f);
    frozen.doubleSided = 1;
    frozen.transform.matrix[0][0] = .25f;
    frozen.transform.matrix[0][3] = -.4f;
    frozen.transform.matrix[1][1] = 3;
    frozen.pNext = &identity;
    const uint64_t handles[] = {wallHandle, r.createRetained(frozen)};
    wall.pNext = nullptr;

    remixapi_LightInfoDistantEXT distant{};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = {0, 0, 1};
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.hash = 0xe106;
    light.pNext = &distant;
    light.radiance = {2, 0, 0};
    remixapi_LightHandle sun;
    r.createLight(light, sun);
    remixapi_BakeSceneBoundsInfoEXT request{};
    request.scene = {sizeof(request), REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT, 0xe107, 1, 1,
        REMIXAPI_BAKE_SCENE_READY_EXT, 2, 1, handles, &sun};
    for (uint32_t c = 0; c < 3; ++c) {
      request.minimum[c] = c == 2 ? 1.5f : -.6f;
      request.maximum[c] = c == 2 ? 2.5f : .6f;
    }
    r.setBakeScene(&request.scene);
    auto weapon = instance(mesh, 1.5f);
    weapon.doubleSided = 1;
    weapon.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
    weapon.transform.matrix[0][0] = weapon.transform.matrix[1][1] = .5f;
    identity.instanceId = 0xe108;
    identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    auto moving = instance(blackMesh, .5f);
    moving.doubleSided = 1;
    moving.transform.matrix[0][0] = .08f;
    moving.transform.matrix[1][1] = .2f;
    moving.pNext = &identity;
    auto render = [&](bool actor = false, bool self = false, bool drawWeapon = true) {
      viewModelDepthCameras(r);
      r.drawInstance(wall);
      if (drawWeapon) r.drawInstance(weapon);
      if (actor || self) {
        auto caster = moving;
        caster.categoryFlags = self ? REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL : 0;
        r.drawInstance(caster);
      }
      r.drawLight(sun);
      return target.render();
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
      render();
      const auto &state = target.forward->statistics().baking;
      require(state.state != REMIXAPI_BAKE_FAILED_EXT, state.status);
      if (state.state == REMIXAPI_BAKE_READY_EXT) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    const auto volume = target.forward->bakeController()->volume();
    require(volume && volume->irradianceContents == f::bake::IrradianceContents::ReceiverDirect &&
                (cascades ? target.forward->statistics().cascades.active : target.forward->statistics().rayGI.active),
            "Viewmodel sunlight fixture publishes direct-only data with live incoming active");
    const size_t centre = (32 * 64 + 32) * 4, left = (32 * 64 + 27) * 4;
    const auto clear = render();
    std::printf("Composed viewmodel sun %ux: static blocked %u, clear %u\n", samples, clear[left], clear[centre]);
    require(clear[left] < clear[centre] / 3 && clear[centre] > 80,
            "Viewmodel receives static sunlight visibility from its world-space probe volume");
    const auto shadow = render(true);
    require(shadow[centre] < clear[centre] / 3 && shadow[left] < 3,
            "Dedicated viewmodel cascade adds moving world shadows to the baked static mask");
    const auto self = render(false, true);
    require(self[centre] < clear[centre] / 3,
            "Viewmodel self shadows remain in the dedicated close-up cascade");
    const auto world = render(false, false, false);
    require(render(false, true, false) == world,
            "A hidden viewmodel self caster cannot shadow the main world");
    require(render() == clear, "Removing viewmodel and world casters restores exact sunlight");
    light.radiance = {0, 0, 2};
    r.updateLight(sun, light);
    const auto blue = render();
    require(blue[centre] < 3 && blue[centre + 2] > 80 && blue[left + 2] < blue[centre + 2] / 3 &&
                target.forward->bakeController()->volume() == volume,
            "Viewmodel sun colour changes reuse the same baked visibility publication");
    const auto previousSource = r.bakeScene();
    ++request.scene.revision;
    r.setBakeScene(&request.scene);
    require(r.bakeScene() != previousSource &&
                r.bakeScene()->contentHash != previousSource->contentHash &&
                r.bakeScene()->receiverDirectHash == previousSource->receiverDirectHash,
            "Explicit sun-power recapture replaces the snapshot but preserves direct content");
    require(render() == blue && target.forward->bakeController()->volume() == volume &&
                target.forward->statistics().baking.state == REMIXAPI_BAKE_READY_EXT,
            "Incoming-only snapshot replacement keeps the exact in-memory direct publication without disk cache");
    weapon.transform.matrix[2][3] = 3.5f; // outside the probe volume
    const auto fallback = render();
    const size_t fallbackLeft = (32 * 64 + 29) * 4;
    require(fallback[fallbackLeft + 2] < fallback[centre + 2] / 3 && fallback[centre + 2] > 80,
            "Uncovered viewmodel receivers retain complete static shadow fallback");
    require(target.forward->bakeController()->volume() == volume && errors.load() == 0,
            "Viewmodel sunlight changes preserve publication and Vulkan synchronization");
  }
}
