// Measure the finite-source gap independently of display curves and probe interpolation.
void sunDepthAngularReference(test::Fixture &fixture, pipeline::Service &pipelines, World &world,
    const scene::BakeScene &source, const bake::SunDepth &field) {
  std::vector<bake::Point> receivers;
  for (float height : {0.f, .75f})
    for (float x : {-.1f, -.05f, 0.f, .05f, .1f})
      receivers.push_back({{x, .425f, height, 0}, {0, 0, 1, 0}});
  const auto captured = querySunDepth(fixture, field.table(), field.values(), receivers);
  for (float diameter : {0.f, .53f, 20.f, 40.f}) {
    auto reference = std::make_shared<scene::BakeScene>(source);
    reference->sun.angularDiameterDegrees = diameter;
    bake::Transport transport;
    bake::Options options;
    options.batchPoints = uint32_t(receivers.size());
    options.bias = .0001f;
    auto preparation = pipelines.prepare("sun-depth-angular-" + std::to_string(diameter), [&] {
      transport.initialize(fixture.physical, fixture.device, reference, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!preparation->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(preparation->ready(), "Angular reference transport preparation completed");
    preparation->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    fixture.finish();
    transport.releaseBuildInputs();
    fixture.begin();
    transport.recordSun(fixture.command, 0, receivers, 8192);
    fixture.finish();
    std::vector<bake::Result> result(receivers.size());
    transport.read(0, result);
    const auto filtered = querySunDepth(fixture, field.table(), field.values(), receivers,
        std::sin(diameter * std::numbers::pi_v<float> / 360));
    for (size_t i = 0; i < receivers.size(); ++i) {
      const float visibility = result[i].irradiance[0][0];
      require(captured[i][1] == 1 && std::isfinite(visibility) && visibility >= 0 && visibility <= 1,
              "Angular reference compares covered receivers with bounded independent visibility");
      std::printf("Sun depth angular reference: diameter=%.3f x=%.3f height=%.3f field=%.6f reference=%.6f filtered=%.6f covered=%.0f\n",
          diameter, receivers[i].position[0], receivers[i].position[2], captured[i][0], visibility, filtered[i][0], filtered[i][1]);
    }
    if (diameter == 0)
      require(filtered == captured, "Point-source filtering preserves the existing lookup exactly");
    if (diameter == 0)
      require(result[2].irradiance[0][0] == 1 && result[7].irradiance[0][0] == 1,
              "Point-source control sees the same open slit at both receiver heights");
    if (diameter == 40)
      require(result[2].irradiance[0][0] < .4f && result[7].irradiance[0][0] > .8f,
              "Finite-source reference resolves wider penumbrae farther from the roof");
  }
  const auto uncovered = querySunDepth(fixture, field.table(), field.values(), receivers, .999f);
  for (const auto &sample : uncovered)
    require(sample[1] == 0, "Wide filters request fallback when their search leaves sparse coverage");
}
