// Included after Target and the common scene helpers in rendering.cpp.
void receiverSubsetReadback() {
  for (uint32_t samples : {1u, 2u, 4u}) {
    Target target(64, 64, true);
    auto &r = target.raster;
    remix::forward::Options options;
    options.prism = false;
    options.samples = samples;
    options.ambient = 0;
    options.shadows = options.localShadows = options.ssr = false;
    options.bakedLighting = true;
    options.baking.lightmapFitBudget = false;
    options.baking.maxProbes = 64;
    options.baking.spacing = 2;
    options.baking.samples = 64;
    options.baking.bounces = 1;
    options.baking.batch = 16;
    options.baking.reflectionProbes = 0;
    options.baking.lightmapPageSize = 32;
    options.baking.lightmapTexels = 1024;
    target.enableForward(options);
    const auto white = material(r, 995001, 1, 1, 1);
    const auto full = quad(r, 995002, white);
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 995003;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    auto instanceInfo = instance(full);
    instanceInfo.pNext = &identity;
    const uint64_t reference = r.createRetained(instanceInfo);
    remixapi_SceneInstanceChangeEXT hidden{};
    hidden.instance = reference;
    hidden.fields = REMIXAPI_SCENE_INSTANCE_VISIBLE_EXT;
    hidden.visible = 0;
    r.updateRetained(&hidden, 1);
    remixapi_LightInfoDomeEXT dome{};
    dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    for (uint32_t c = 0; c < 3; ++c) dome.transform.matrix[c][c] = 1;
    remixapi_LightInfo light{};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &dome;
    light.hash = 995004;
    light.radiance = {0, 1, 0};
    remixapi_LightHandle environment;
    r.createLight(light, environment);
    const float uv[]{0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
    const remixapi_BakeSurfaceChartEXT chart{reference, 0, 0, 0, 6, 9, 9, uv};
    remixapi_BakeSceneReceiversInfoEXT request{};
    auto &surfaces = request.styled.reflections.surfaces;
    auto &base = surfaces.bounds.scene;
    base = {sizeof(request), REMIXAPI_BAKE_SCENE_RECEIVERS_VERSION_EXT, 995, 1, 1,
            REMIXAPI_BAKE_SCENE_READY_EXT, 1, 1, &reference, &environment};
    for (uint32_t c = 0; c < 3; ++c) {
      surfaces.bounds.minimum[c] = 10;
      surfaces.bounds.maximum[c] = 11; // no probe can cover the rendered surface
    }
    surfaces.chartRevision = 1;
    surfaces.chartCount = 1;
    surfaces.charts = &chart;
    request.receiverCount = 1;
    request.receivers = &reference;
    r.setBakeScene(&base);
    auto render = [&] {
      camera(r);
      r.drawRetained(nullptr);
      r.drawInstance(instanceInfo);
      return target.render();
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::vector<uint8_t> baseline;
    do {
      baseline = render();
      const auto &status = target.forward->statistics().baking;
      require(status.state != REMIXAPI_BAKE_FAILED_EXT, "Receiver-only readback bake succeeds");
      if (status.state == REMIXAPI_BAKE_READY_EXT) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    require(target.forward->statistics().baking.state == REMIXAPI_BAKE_READY_EXT,
            "Receiver-only readback reaches a completed bake");
    const auto volume = target.forward->bakeController()->volume();
    const auto content = target.forward->statistics().bakeContent;
    const size_t left = (32 * 64 + 16) * 4, right = (32 * 64 + 48) * 4;
    require(baseline[left + 1] > 80 && baseline[right + 1] > 80,
            "Invisible receiver reference supplies surface lighting beyond probe coverage");
    for (uint32_t variant = 0; variant < 4; ++variant) {
      remixapi_HardcodedVertex vertices[4]{};
      const float positions[4][3]{{-.8f, -.8f, .5f}, {.8f, -.8f, .5f}, {.8f, .8f, .5f}, {-.8f, .8f, .5f}};
      for (uint32_t i = 0; i < 4; ++i) {
        std::copy_n(positions[i], 3, vertices[i].position);
        vertices[i].color = 0xffffffff;
      }
      const uint32_t rightIndices[]{2, 1, 0}, leftIndices[]{3, 2, 0};
      remixapi_MeshInfoSurfaceTriangles surface{};
      surface.vertices_count = 4;
      surface.vertices_values = vertices;
      surface.indices_count = 3;
      surface.indices_values = variant % 2 ? leftIndices : rightIndices;
      surface.material = white;
      remixapi_MeshInfo mesh{};
      mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
      mesh.hash = 995010 + variant;
      mesh.surfaces_count = 1;
      mesh.surfaces_values = &surface;
      remixapi_MeshHandle handle;
      r.createMesh(mesh, handle);
      instanceInfo.mesh = handle;
      identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
      const auto pixels = render();
      const size_t lit = variant % 2 ? left : right, empty = variant % 2 ? right : left;
      require(std::abs(int(pixels[lit + 1]) - int(baseline[lit + 1])) <= 2 && pixels[empty + 1] < 3,
              "Fresh dynamic index subsets receive the same baked surface light without drawing omitted triangles");
      require(target.forward->bakeController()->volume() == volume &&
                  target.forward->statistics().bakeContent == content,
              "Live index subset changes retain the authoritative completed bake");
    }
    remixapi_InstanceInfoMaterialEXT tint{};
    tint.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT;
    tint.pNext = &identity;
    tint.material = material(r, 995100, 1, .25f, 1);
    instanceInfo.pNext = &tint;
    const auto edited = render();
    require(edited[left + 1] > 20 && edited[left + 1] < baseline[left + 1] * .8f &&
                target.forward->bakeController()->volume() == volume,
            "Live receiver material edits modulate baked irradiance without rebuilding the bake");
    instanceInfo.transform.matrix[0][3] = .1f;
    const auto moved = render();
    require(moved[left + 1] < 3 && moved[right + 1] < 3,
            "A moved receiver falls back instead of borrowing light from its old placement");
  }
}
