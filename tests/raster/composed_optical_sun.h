// Optical direct-light checks; black transmission background isolates body light.
void composedOpticalSunTests(bool cascades = false) {
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
    options.rtGIDistance = .1f;
    options.rtReflections = samples == 2; // qualify both composed water pipeline variants
    options.rtReflectionDistance = .1f;
    options.rtReflectionBounces = 1;
    options.water = true;
    options.prism = options.gamma22 = options.ssr = options.localShadows = false;
    options.ambient = 0;
    options.shadowResolution = 512;
    options.shadowDistance = .11f; // water lies beyond every realtime cascade
    options.baking.spacing = .125f;
    options.baking.maxProbes = 4096;
    options.baking.samples = options.baking.batch = 64;
    options.baking.bounces = 1;
    options.baking.reflectionProbes = 0;
    options.baking.diskCache = options.baking.denoiseLightmaps = false;
    target.enableForward(options);
    // A black dielectric still reflects the sun and would contaminate the
    // transmitted background. Black metal supplies neither diffuse nor F0.
    remixapi_MaterialInfoOpaqueEXT blackSurface{};
    blackSurface.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    blackSurface.albedoConstant = {0, 0, 0};
    blackSurface.opacityConstant = blackSurface.roughnessConstant = blackSurface.metallicConstant = 1;
    blackSurface.alphaTestType = 7;
    remixapi_MaterialInfo blackInfo{};
    blackInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    blackInfo.hash = 0xf101;
    blackInfo.pNext = &blackSurface;
    remixapi_MaterialHandle black;
    r.createMaterial(blackInfo, black);
    const auto blackMesh = quad(r, 0xf100, black);
    remixapi_MaterialInfoTranslucentEXT optical{};
    optical.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    optical.refractiveIndex = 1; // zero Fresnel isolates diffuse body response
    optical.transmittanceColor = {.5f, .5f, .5f};
    optical.transmittanceMeasurementDistance = 1;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.hash = 0xf102;
    info.pNext = &optical;
    remixapi_MaterialHandle liquidMaterial;
    r.createMaterial(info, liquidMaterial);
    const auto liquidMesh = quad(r, 0xf103, liquidMaterial);
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 0xf104;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 0;
    auto frozen = instance(blackMesh, -.4f);
    frozen.doubleSided = 1;
    frozen.transform.matrix[0][0] = .4f;
    frozen.transform.matrix[0][3] = -.5f;
    frozen.transform.matrix[1][1] = 2;
    frozen.pNext = &identity;
    const auto handle = r.createRetained(frozen);
    remixapi_LightInfoDistantEXT distant{};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = {0, 0, 1};
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.hash = 0xf105;
    light.pNext = &distant;
    light.radiance = {8, 0, 0};
    remixapi_LightHandle sun;
    r.createLight(light, sun);
    remixapi_BakeSceneBoundsInfoEXT request{};
    request.scene = {sizeof(request), REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT, 0xf106, 1, 1,
        REMIXAPI_BAKE_SCENE_READY_EXT, 1, 1, &handle, &sun};
    request.minimum[0] = request.minimum[1] = -1;
    request.maximum[0] = request.maximum[1] = 1;
    request.minimum[2] = .2f;
    request.maximum[2] = .4f;
    r.setBakeScene(&request.scene);
    identity.instanceId = 0xf107;
    identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    auto moving = instance(blackMesh, -.35f);
    moving.doubleSided = 1;
    moving.transform.matrix[0][0] = moving.transform.matrix[1][1] = .08f;
    moving.pNext = &identity;
    float surfaceOffset = -.2f;
    auto render = [&](bool actor = false) {
      camera(r);
      r.drawInstance(instance(blackMesh, .3f));
      auto liquid = instance(liquidMesh, surfaceOffset);
      liquid.transform.matrix[0][0] = liquid.transform.matrix[1][1] = .6f;
      r.drawInstance(liquid);
      if (actor) r.drawInstance(moving);
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
            "Optical sunlight fixture publishes direct-only data with live incoming active");
    const size_t centre = (32 * 64 + 32) * 4, left = (32 * 64 + 24) * 4;
    const auto clear = render();
    std::printf("Composed optical sun %ux: static blocked %u, clear %u\n", samples, clear[left], clear[centre]);
    require(clear[centre] > 40 && clear[left] < clear[centre] / 3,
            "Optical body lighting uses baked static sunlight beyond realtime cascades");
    options.shadowDistance = 3;
    target.forward->updateLighting(options);
    const auto shadow = render(true);
    std::printf("Composed optical moving %ux: static blocked %u, moving blocked %u\n",
                samples, shadow[left], shadow[centre]);
    require(shadow[centre] < clear[centre] / 3 && shadow[left] == clear[left],
            "Optical sunlight combines moving-caster shadows with the static probe mask");
    const auto restored = render();
    require(restored == clear, "Removing an optical sun caster restores exact body lighting");
    light.radiance = {0, 0, 8};
    r.updateLight(sun, light);
    const auto blue = render();
    require(blue[centre] < 3 && blue[centre + 2] > 40 && blue[left + 2] < blue[centre + 2] / 3 &&
                target.forward->bakeController()->volume() == volume,
            "Optical sun colour changes retain the captured unit visibility field");
    surfaceOffset = .1f; // outside the probe volume; full static CSM must remain valid
    const auto fallback = render();
    require(fallback[centre + 2] > 20 && fallback[left + 2] < fallback[centre + 2] / 3,
            "Uncovered optical receivers retain complete static shadow fallback");
    require(target.forward->bakeController()->volume() == volume && errors.load() == 0,
            "Optical sunlight changes preserve publication and Vulkan synchronization");
  }
}
