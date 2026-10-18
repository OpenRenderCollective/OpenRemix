void indexedVisibilityTests(test::Fixture &fixture, pipeline::Service &pipelines, World &world,
                            const std::shared_ptr<scene::BakeScene> &original) {
  auto source = std::make_shared<scene::BakeScene>(*original);
  auto second = source->lights.at(0);
  second.id += 123;
  second.position[0] += .8f;
  second.radius = .1f;
  second.coneCos = .2f;
  source->lights.push_back(second);
  std::vector<bake::Point> receivers(257);
  for (uint32_t i = 0; i < receivers.size(); ++i) {
    receivers[i].position = {-.9f + 1.8f * float(i % 17) / 16, -.8f + 1.6f * float(i / 17) / 15, 0, 99};
    receivers[i].normal = {0, 0, -1, float(i % 3)};
  }
  bake::Transport indexed, reference;
  bake::Options options;
  options.batchPoints = bake::Budget::maximumPoints;
  options.initialBatchPoints = 4;
  options.rayBudget = bake::Budget::maximumWork;
  bool supported = false;
  auto job = pipelines.prepare("indexed-visibility-receivers", [&] {
    indexed.initialize(fixture.physical, fixture.device, source, options);
    reference.initialize(fixture.physical, fixture.device, source, options);
    require(!reference.prepareVisibilityReceivers(receivers, 0) && !reference.hasVisibilityReceivers(),
            "A receiver memory budget decline retains the ordinary CPU path");
    auto invalid = receivers[0];
    invalid.normal[2] = std::numeric_limits<float>::infinity();
    bool refused = false;
    try { indexed.prepareVisibilityReceivers({&invalid, 1}); }
    catch (const std::invalid_argument &) { refused = true; }
    require(refused, "Receiver validation rejects non-finite data before receiver allocation");
    supported = indexed.prepareVisibilityReceivers(receivers);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!job->ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(job->ready(), "Indexed visibility preparation completed");
  job->check();
  if (!supported) {
    std::puts("Indexed visibility unavailable; ordinary transport remains supported");
    return;
  }
  require(indexed.visibilityReceiverBytes() >= receivers.size() * sizeof(bake::Point) &&
              indexed.visibilityReceiverBytes() <= (128ull << 20),
          "The immutable receiver table has an explicit host memory bound");
  const auto expectedReceivers = receivers;
  receivers[0].position[0] += 100;
  fixture.begin();
  indexed.recordPrepare(fixture.command, world.resources);
  reference.recordPrepare(fixture.command, world.resources);
  fixture.finish();
  indexed.releaseBuildInputs();
  reference.releaseBuildInputs();
  auto compare = [&](uint32_t count, uint32_t samples, uint32_t slot) {
    std::vector<bake::VisibilityTask> tasks(count);
    std::vector<bake::VisibilityPoint> expanded(count);
    for (uint32_t i = 0; i < count; ++i) {
      tasks[i] = {(i * 73) % uint32_t(receivers.size()), i % 2, i % 4};
      expanded[i] = {expectedReceivers[tasks[i].point], tasks[i].light};
    }
    fixture.begin();
    indexed.recordIndexedVisibility(fixture.command, slot, tasks, samples);
    reference.recordVisibility(fixture.command, slot, expanded, samples, false);
    fixture.finish();
    std::vector<float> actual(count), expected(count);
    indexed.readScalarVisibility(slot, actual);
    reference.readScalarVisibility(slot, expected);
    require(std::memcmp(actual.data(), expected.data(), count * sizeof(float)) == 0,
            "Prevalidated CPU receiver packing preserves exact visibility, task order and independent light selection");
    // Reusing the same slot for the CPU path must restore its ordinary layout.
    fixture.begin();
    indexed.recordVisibility(fixture.command, slot, expanded, samples, false);
    fixture.finish();
    indexed.readScalarVisibility(slot, actual);
    require(std::memcmp(actual.data(), expected.data(), count * sizeof(float)) == 0,
            "A completed indexed slot can return to ordinary CPU point input");
  };
  for (uint32_t samples : {1u, 63u, 65u, 256u}) {
    compare(127, samples, 0);
    compare(509, samples, 1);
  }
  compare(bake::Budget::maximumPoints - 3, 1, 0);
  for (auto task : {bake::VisibilityTask{uint32_t(receivers.size()), 0, 0},
                    bake::VisibilityTask{0, uint32_t(source->lights.size()), 0}}) {
    bool refused = false;
    try { indexed.recordIndexedVisibility(VK_NULL_HANDLE, 0, {&task, 1}, 1); }
    catch (const std::invalid_argument &) { refused = true; }
    require(refused, "Invalid task indices are refused before recording GPU commands");
  }
  bool replacementRefused = false;
  try { indexed.prepareVisibilityReceivers(receivers); }
  catch (const std::logic_error &) { replacementRefused = true; }
  require(replacementRefused, "Prepared receiver tables cannot be replaced while submissions may reference them");
  std::puts("Indexed visibility: 257 receivers, two lights, 1/63/65/256 samples, alternating slots and 2D tail match exactly");
}
