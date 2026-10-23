// Included inside rendering.cpp's fixture namespace.
void sunDepthLifecycleTests() {
  namespace f = remix::forward;
  require(std::getenv("REMIX_DEBUG_BAKE_SUN_DEPTH"), "Sun depth lifecycle explicitly enables the optional field");
  Target target(64, 64, true);
  auto &r = target.raster;
  f::Options options;
  options.ambient = 0;
  options.ssr = options.localShadows = false;
  options.shadowResolution = 128;
  options.baking.lightmapFitBudget = false;
  options.baking.maxProbes = 64;
  options.baking.samples = 64;
  options.baking.bounces = 1;
  options.baking.batch = 512;
  options.baking.gpuBudgetMs = 0;
  options.baking.reflectionProbes = 0;
  options.baking.lightmapPageSize = 32;
  options.baking.diskCache = false;
  target.enableForward(options);
  const auto mesh = quad(r, 0xda00, material(r, 0xda01, 1, 1, 1));
  auto receiver = instance(mesh);
  const uint64_t retained = r.createRetained(receiver);
  auto blocker = instance(mesh, -.4f);
  blocker.doubleSided = 1;
  blocker.transform.matrix[0][0] = .5f;
  blocker.transform.matrix[0][3] = -.4f;
  const uint64_t instances[]{retained, r.createRetained(blocker)};
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {0, 0, 1};
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &distant;
  light.hash = 0xda02;
  light.radiance = {1, .1f, .1f};
  remixapi_LightHandle sun;
  r.createLight(light, sun);
  const float uv[]{0, 0, 8, 8, 8, 0, 0, 0, 0, 8, 8, 8};
  const remixapi_BakeSurfaceChartEXT chart{retained, 0, 0, 0, 6, 9, 9, uv};
  remixapi_BakeSceneSurfacesInfoEXT request{};
  auto &base = request.bounds.scene;
  base = {sizeof(request), REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT, 0xda03, 1, 1,
          REMIXAPI_BAKE_SCENE_READY_EXT, 2, 1, instances, &sun};
  std::fill_n(request.bounds.minimum, 3, -1.f);
  std::fill_n(request.bounds.maximum, 3, 1.f);
  request.chartRevision = request.chartCount = 1;
  request.charts = &chart;
  auto render = [&] {
    camera(r);
    // An unmapped copy exercises the field through ordinary fragment shading.
    r.drawInstance(instance(mesh));
    r.drawLight(sun);
    return target.render();
  };
  bool tracedDepth = false;
  auto until = [&](bool ready, const char *phase = "Baking sparse sun depth") {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    std::vector<uint8_t> pixels;
    do {
      pixels = render();
      const auto &bake = target.forward->statistics().baking;
      if (bake.state == REMIXAPI_BAKE_FAILED_EXT) std::fprintf(stderr, "%s\n", bake.status);
      require(bake.state != REMIXAPI_BAKE_FAILED_EXT, "Sun depth controller bake succeeds");
      const bool depth = std::strstr(bake.status, "Baking sparse sun depth") != nullptr;
      tracedDepth |= depth;
      if (ready ? bake.active && bake.state == REMIXAPI_BAKE_READY_EXT : std::strstr(bake.status, phase) != nullptr) return pixels;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    require(false, "Sun depth controller reaches requested phase");
    return pixels;
  };
  auto unload = [&] {
    r.setBakeScene(nullptr);
    for (uint32_t i = 0; i < remix::gpuFrameCount + 2; ++i) render();
    require(!target.forward->statistics().baking.active && !target.forward->bakeController()->volume(),
            "Unload clears the controller publication after all frame slots");
  };
  r.setBakeScene(&base);
  until(false);
  unload(); // Supersede a capture with recorded batches and pending fence receipts.
  options.baking.diskCache = true;
  target.forward->updateLighting(options);
  ++base.revision;
  r.setBakeScene(&base);
  until(true);
  const auto first = target.forward->bakeController()->volume();
  require(first && first->sunDepth && !first->sunDepth->restored(), "Cold publication owns newly captured depth");
  // Change the frozen light, retain the old volume as an outstanding frame owner.
  light.radiance = {.1f, .1f, 1};
  r.updateLight(sun, light);
  ++base.sceneId;
  ++base.revision;
  r.setBakeScene(&base);
  const auto replacementPixels = until(true);
  const auto replacement = target.forward->bakeController()->volume();
  require(replacement && replacement != first && replacement->sunDepth &&
              replacement->sunDepth->values().buffer != first->sunDepth->values().buffer &&
              target.forward->statistics().baking.activeHash == r.bakeScene()->contentHash,
          "Only the replacement scene publishes, with independent depth resources");
  f::bake::cache::persistence().waitIdle();
  unload();
  light.radiance = {.1f, 1, .1f};
  r.updateLight(sun, light);
  ++base.revision;
  r.setBakeScene(&base);
  until(false, "Saving sparse sun depth cache");
  unload(); // Also cancel while cache-copy receipts or the writer may own the field.
  light.radiance = {.1f, .1f, 1};
  r.updateLight(sun, light);
  tracedDepth = false;
  ++base.revision;
  r.setBakeScene(&base);
  const auto warmPixels = until(true);
  const auto warm = target.forward->bakeController()->volume();
  require(warm && warm->sunDepth && warm->sunDepth->restored() && !tracedDepth,
          "Warm scene publication restores depth without entering capture");
  require(warmPixels == replacementPixels, "Warm restored depth preserves fragment output exactly");
  options.samples = 4;
  target.enableForward(options, true);
  render();
  require(target.forward->bakeController()->volume() == warm,
          "Attachment replacement retains the published sun depth owner");
  unload();
  require(errors.load() == 0, "Sun depth cancellation, replacement, restore and unload are validation clean");
  std::puts("Sun depth lifecycle: cancelled capture/save, replacement, warm restore, target replacement and unload passed");
}
