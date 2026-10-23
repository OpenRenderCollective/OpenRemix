// Production owner coverage: moving receivers and retained resources across replacement/cancellation.
void sunDepthOwnerLifetime(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
  const auto material = world.material({});
  const auto left = world.quad(material, {{{-2,-2,1},{-.075f,-2,1},{-.075f,2,1},{-2,2,1}}});
  const auto right = world.quad(material, {{{.075f,-2,1},{2,-2,1},{2,2,1},{.075f,2,1}}});
  const auto floor = world.quad(material, {{{-2,-2,-1},{2,-2,-1},{2,2,-1},{-2,2,-1}}});
  auto publishedScene = world.scene({left, right, floor});
  publishedScene->sun.isDistant = true;
  publishedScene->sun.direction[2] = -1;
  publishedScene->sun.angularDiameterDegrees = 0;
  publishedScene->charts.push_back({2, 0, 3, 3, {{0,0},{2,2},{2,0},{0,0},{0,2},{2,2}}});
  const auto receiverLayout = lightmap::plan(*publishedScene, {16, 1, 2, 256});
  auto retained = bake::SunDepth::prepare(fixture.physical, fixture.device, *publishedScene, receiverLayout, {});
  require(retained && !retained->restored() && !retained->cacheEnabled(),
          "Production sun depth prepares complete coverage without requiring a disk cache");
  bake::Transport ownerTransport;
  bake::Options ownerOptions;
  ownerOptions.depthCapture = true;
  ownerOptions.bias = .0001f;
  ownerOptions.batchPoints = 65536;
  auto ownerPreparation = pipelines.prepare("sun-depth-owner-lifetime", [&] {
    ownerTransport.initialize(fixture.physical, fixture.device, publishedScene, ownerOptions);
  });
  const auto ownerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!ownerPreparation->ready() && std::chrono::steady_clock::now() < ownerDeadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(ownerPreparation->ready(), "Production owner transport preparation completed");
  ownerPreparation->check();
  fixture.begin();
  ownerTransport.recordPrepare(fixture.command, world.resources);
  fixture.finish();
  ownerTransport.releaseBuildInputs();
  for (uint32_t first = 0; first < retained->tasks(); first += ownerOptions.batchPoints) {
    const auto batch = retained->batch(first, std::min(ownerOptions.batchPoints, retained->tasks()-first));
    fixture.begin();
    ownerTransport.recordIndexedDepth(fixture.command, 0, batch, retained->values(), retained->distance());
    fixture.finish();
  }
  std::vector<bake::Point> moving;
  for (float x : {-.5f, 0.f, .5f}) moving.push_back({{x,.425f,0,0},{}});
  const auto original = querySunDepth(fixture, retained->table(), retained->values(), moving);
  require(original[0] == std::array<float,2>{0,1} && original[1] == std::array<float,2>{1,1} &&
              original[2] == std::array<float,2>{0,1},
          "Moving receivers cross the captured roof opening without changing the field");
  auto replacementScene = *publishedScene;
  replacementScene.sun.direction[0] = 1;
  replacementScene.sun.direction[2] = 0;
  auto replacement = bake::SunDepth::prepare(fixture.physical, fixture.device, replacementScene, receiverLayout, {});
  require(replacement && replacement->values().buffer != retained->values().buffer &&
              replacement->table().buffer != retained->table().buffer,
          "A replacement scene owns independent depth and lookup allocations");
  replacement.reset();
  require(querySunDepth(fixture, retained->table(), retained->values(), moving) == original,
          "Releasing a replacement leaves retained-frame sun depth intact");
  auto oversizedScene = *publishedScene;
  oversizedScene.draws[2].model[0] = oversizedScene.draws[2].model[5] = 1000000;
  require(!bake::SunDepth::prepare(fixture.physical, fixture.device, oversizedScene, receiverLayout, {}),
          "Over-budget optional coverage declines without failing the lighting bake");
  std::stop_source cancelled;
  cancelled.request_stop();
  bool stopped = false;
  try { bake::SunDepth::prepare(fixture.physical, fixture.device, *publishedScene, receiverLayout, cancelled.get_token()); }
  catch (const std::runtime_error &) { stopped = true; }
  require(stopped && querySunDepth(fixture, retained->table(), retained->values(), moving) == original,
          "Cancelled scene preparation preserves previously published depth");
  std::puts("Sun depth owner: moving receivers, independent replacement and cancellation passed");
}
