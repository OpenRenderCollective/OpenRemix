// Included after World: exercise planning -> hardware scalar visibility -> sparse
// ownership, using the same public transport calls needed by the controller.
void localProbeVisibilityTests(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
  auto source = world.scene({world.wall(world.material({}), 0, 2)});
  for (uint32_t i = 0; i < 6; ++i) {
    scene::SphereLight light;
    light.id = 500 + i;
    light.position[2] = 2;
    light.radius = .1f;
    light.radiance[0] = light.radiance[1] = light.radiance[2] = 1;
    light.style = i == 4 ? 32 : 0;
    light.dynamic = i == 5;
    source->lights.push_back(light);
  }
  bake::Grid grid;
  grid.header.originSpacing[3] = .25f;
  grid.points = {{{0, 0, -1, 0}, {}}, {{0, 0, 1, 0}, {}}, {{4, 0, -2, 0}, {}}};
  grid.probes.resize(3);
  for (auto &probe : grid.probes)
    probe.statistics[3] = 1;
  const auto plan = bake::planProbeVisibility(grid, source->lights, .001f, 15);
  require(plan && plan->tasks.size() == 15, "Five stationary lights schedule blocked, clear and partial probes");
  bake::ProbeVisibility visibility(3, 6, plan->tasks, 1024);
  bake::Transport transport;
  bake::Options options;
  options.batchPoints = 4;
  options.initialBatchPoints = 1;
  options.textureResolution = 32;
  options.queueFamily = fixture.family;
  auto prepared = pipelines.prepare("local-probe-visibility-test", [&] {
    transport.initialize(fixture.physical, fixture.device, source, options);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!prepared->ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(prepared->ready(), "Local probe visibility transport preparation completed");
  prepared->check();
  fixture.begin();
  transport.recordPrepare(fixture.command, world.resources);
  fixture.finish();
  transport.releaseBuildInputs();
  auto rejects = [&](std::span<const bake::Point> points, std::span<const bake::VisibilityTask> tasks,
                     uint32_t samples) {
    bool rejected = false;
    fixture.begin();
    try {
      transport.recordVisibilityTasks(fixture.command, 0, points, tasks, samples);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    fixture.finish();
    require(rejected, "Invalid local probe task batch is rejected before dispatch");
  };
  auto invalidPoints = grid.points;
  invalidPoints[0].normal[0] = std::numeric_limits<float>::infinity();
  const auto singleTask = std::span(plan->tasks).first(1);
  rejects(invalidPoints, singleTask, 256);
  rejects(grid.points, singleTask, 0);
  const bake::VisibilityTask invalidProbe{3, 0, 0}, invalidLight{0, 6, 0};
  rejects(grid.points, {&invalidProbe, 1}, 256);
  rejects(grid.points, {&invalidLight, 1}, 256);
  rejects(grid.points, {}, 256);
  for (size_t first = 0; first < plan->tasks.size(); first += 4) {
    const auto tasks = std::span(plan->tasks).subspan(first, std::min<size_t>(4, plan->tasks.size() - first));
    std::vector<bake::VisibilityPoint> expanded;
    for (const auto &task : tasks)
      expanded.push_back({grid.points[task.point], task.light});
    fixture.begin();
    transport.recordVisibilityTasks(fixture.command, 0, grid.points, tasks, 256);
    transport.recordVisibility(fixture.command, 1, expanded, 256);
    fixture.finish();
    std::vector<float> actual(tasks.size()), reference(tasks.size());
    transport.readScalarVisibility(0, actual);
    transport.readScalarVisibility(1, reference);
    require(actual == reference, "Local probe task masks match ordinary visibility byte for byte");
    visibility.observe(tasks, actual);
  }
  bake::ProbeVisibility directional(3, 6, plan->tasks, 4096, true);
  for (size_t first = 0; first < plan->tasks.size(); first += 4) {
    const auto tasks = std::span(plan->tasks).subspan(first, std::min<size_t>(4, plan->tasks.size() - first));
    std::vector<bake::VisibilityPoint> expanded;
    for (const auto &task : tasks) expanded.push_back({grid.points[task.point], task.light});
    fixture.begin();
    transport.recordVisibilityTasks(fixture.command, 0, grid.points, tasks, 256, true);
    transport.recordVisibility(fixture.command, 1, expanded, 256, true);
    fixture.finish();
    std::vector<bake::VisibilityResult> actual(tasks.size()), reference(tasks.size());
    transport.readVisibility(0, actual);
    transport.readVisibility(1, reference);
    for (size_t i = 0; i < actual.size(); ++i)
      require(actual[i].visibility == reference[i].visibility && actual[i].irradiance == reference[i].irradiance,
              "Indexed probe SH matches ordinary hardware direct visibility without additional rays");
    directional.observe(tasks, actual);
  }
  require(directional.sampleDirect(0, 0) && (*directional.sampleDirect(0, 0))[0] == 0 &&
              directional.sampleDirect(1, 0) && (*directional.sampleDirect(1, 0))[0] > 0,
          "Probe directional attribution distinguishes blocked and clear finite emitters");
  for (uint32_t light = 0; light < 5; ++light) {
    require(visibility.sample(0, light) == 0, "Opaque static wall blocks local probe visibility");
    require(visibility.sample(1, light) == 1, "Unblocked local probe retains clear visibility");
    const auto partial = visibility.sample(2, light);
    require(partial && *partial > .25f && *partial < .75f,
            "Wall edge retains a soft partial visibility fraction for finite local emitters");
  }
  require(!visibility.sample(0, 5) && !visibility.sample(1, 5),
          "Moving light remains explicitly unknown, retaining ordinary dynamic fallback");
  require(!transport.hasVisibilityReceivers(),
          "Caller-owned probe tasks need no second immutable receiver copy");
  std::puts("Local probe hardware visibility: five blocked/clear/partial lights, styled identity, scalar parity and "
            "tail batches passed");
}
