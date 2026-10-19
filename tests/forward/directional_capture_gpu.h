// Included after World. Exercise real transport -> streamed collection -> indexed fields.
// Keep the fixture out of the large transport main; Clang 19's loop-strength
// reduction crashes when this additional mixed-dispatch check is inlined there.
[[gnu::noinline]] void directionalCaptureGpuTests(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
  const auto receiver = world.wall(world.material({}), 0, 2);
  const auto blocker = world.quad(world.material({}),
      {{{-3, -3, -.5f}, {0, -3, -.5f}, {0, 3, -.5f}, {-3, 3, -.5f}}});
  auto source = world.scene({receiver, blocker});
  for (uint32_t i = 0; i < 7; ++i) {
    scene::SphereLight light;
    light.id = 0x8500 + i;
    light.position[0] = float(int(i) - 3) * .05f;
    light.position[2] = -1.5f;
    light.radius = .1f + .01f * i;
    for (auto &channel : light.radiance) channel = 1;
    source->lights.push_back(light);
  }
  lightmap::Layout layout;
  layout.pageSize = 8; layout.pages = 1;
  layout.charts = {{0, 1, 1, 2, 1}};
  layout.points = {{{-.75f, 0, 0, 0}, {0, 0, -1, 0}}, {{.75f, 0, 0, 0}, {0, 0, -1, 0}}};
  layout.pointCharts = {0, 0};
  layout.owners.assign(64, lightmap::unowned);
  layout.owners[9] = 0; layout.owners[10] = 1;
  std::vector<mixed::DirectionalCapture::Link> links;
  for (uint32_t i = 0; i < 7; ++i) links.push_back({0, i});
  mixed::DirectionalCapture capture(layout, 7, links, 4096);
  bake::Transport transport;
  bake::Options options;
  options.textureResolution = 32; options.batchPoints = 14; options.queueFamily = fixture.family;
  auto prepared = pipelines.prepare("streamed-directional-capture-test", [&] {
    transport.initialize(fixture.physical, fixture.device, source, options);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!prepared->ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(prepared->ready(), "Streamed directional transport preparation completed");
  prepared->check();
  fixture.begin(); transport.recordPrepare(fixture.command, world.resources); fixture.finish();
  transport.releaseBuildInputs();
  std::vector<std::array<bake::VisibilityResult, 2>> reference;
  for (uint32_t light = 0; light < 7; ++light) {
    require(capture.begin() == 2, "Every light retains both actual surface receivers");
    const auto tasks = capture.batch(0, 2);
    const bake::VisibilityPoint points[] = {{layout.points[tasks[0].point], tasks[0].light},
                                          {layout.points[tasks[1].point], tasks[1].light}};
    fixture.begin(); transport.recordVisibility(fixture.command, 0, points, 1024, true); fixture.finish();
    std::array<bake::VisibilityResult, 2> results;
    transport.readVisibility(0, results);
    require(results[0].visibility == 0 && results[1].visibility == 1 && results[1].irradiance[0] > 0,
            "Hardware capture distinguishes the shadowed and exposed texels beyond four owners");
    reference.push_back(results);
    capture.observe(1, std::span(results).last(1));
    capture.observe(0, std::span(results).first(1));
    require(capture.finish(), "A complete hardware field fits the indexed publication budget");
  }
  const auto words = capture.words();
  {
    mixed::DirectionalCapture windowed(layout, 7, links, 4096);
    require(windowed.beginWindow(14) == 14 && windowed.window().size() == 7,
            "One hardware window contains seven independent chart/light owners");
    std::vector<uint32_t> indices;
    for (uint32_t i = 14; i > 0; --i) indices.push_back(i - 1);
    const auto tasks = windowed.batch(indices);
    std::vector<bake::VisibilityPoint> points;
    for (const auto &task : tasks) points.push_back({layout.points[task.point], task.light});
    fixture.begin(); transport.recordVisibility(fixture.command, 0, points, 1024, true); fixture.finish();
    std::vector<bake::VisibilityResult> results(points.size());
    transport.readVisibility(0, results);
    for (size_t i = 0; i < tasks.size(); ++i) {
      const auto &expected = reference[tasks[i].light][tasks[i].point];
      require(results[i].visibility == expected.visibility,
              "Reordering receivers/lights across hardware batches preserves visibility exactly");
      for (uint32_t c = 0; c < 9; ++c)
        require(results[i].irradiance[c] == expected.irradiance[c],
                "Hardware sample seeds and signed coefficients are independent of dispatch order");
    }
    windowed.observe(indices, results);
    require(windowed.finish() && windowed.words() == words,
            "One reversed mixed-light GPU dispatch encodes exactly the serial field reference");
  }
  mixed::DirectionalFields::validate(words, 1, 7, mixed::DirectionalFields::maximumBytes);
  for (uint32_t light = 0; light < 7; ++light)
    for (uint32_t point = 0; point < 2; ++point) {
      const auto field = mixed::DirectionalFields::sample(words, 0, light, point, 0);
      require(bool(field), "Every overlapping hardware emitter remains an explicit field owner");
      for (uint32_t c = 0; c < 9; ++c)
        require((*field)[c] == half::encode(reference[light][point].irradiance[c]),
                "Encoded fields preserve each signed hardware coefficient under its captured light");
    }
}
