// Focused GPU prototype: capture sparse orthographic depth once, then compare
// CPU atlas lookup with independent hardware sun visibility at two heights.
void sparseSunDepthPrototype(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
  const auto material = world.material({});
  const auto left = world.quad(material, {{{-2,-2,1},{-.075f,-2,1},{-.075f,2,1},{-2,2,1}}});
  const auto right = world.quad(material, {{{.075f,-2,1},{2,-2,1},{2,2,1},{.075f,2,1}}});
  const auto cutout = world.wall(world.material({.opacity = 0, .cutout = true}), 1.25f, 2);
  auto scene = world.scene({left, right, cutout});
  scene->sun.isDistant = true;
  scene->sun.direction[2] = -1;
  scene->sun.angularDiameterDegrees = 0;
  bake::Options transportOptions;
  transportOptions.batchPoints = 256;
  transportOptions.distance = 8;
  transportOptions.bias = .0001f;
  bake::Transport transport;
  auto preparation = pipelines.prepare("sparse-sun-depth-prototype", [&] {
    transport.initialize(fixture.physical, fixture.device, scene, transportOptions);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
  while (!preparation->ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(preparation->ready(), "Sparse sun depth transport preparation completed");
  preparation->check();
  fixture.begin();
  transport.recordPrepare(fixture.command, world.resources);
  fixture.finish();
  transport.releaseBuildInputs();
  bake::SunDepthLayoutOptions options;
  options.pageSize = 64; options.tileTexels = 20; options.border = 2;
  options.maximumPages = 1; options.maximumBytes = 64 * 64 * 4 + 16;
  const std::array<std::array<int32_t,2>,4> tiles{{{-1,-1},{0,-1},{-1,0},{0,0}}};
  const auto planned = bake::planSunDepthLayout(tiles, options);
  require(bool(planned), "Sparse sun depth prototype fits its explicit atlas budget");
  const auto &layout = *planned;
  constexpr uint32_t extent = 24;
  const float texel = 1.f / options.tileTexels;
  std::vector<float> depth(options.pageSize * options.pageSize, 8);
  std::vector<bake::Point> rays;
  std::vector<uint32_t> destinations;
  uint32_t hits = 0, misses = 0;
  auto flush = [&] {
    if (rays.empty()) return;
    fixture.begin();
    transport.recordGeometry(fixture.command, 0, rays);
    fixture.finish();
    std::vector<bake::Result> results(rays.size());
    transport.read(0, results);
    for (size_t i = 0; i < results.size(); ++i) {
      const auto &r = results[i];
      const bool miss = r.statistics[1] > .5f;
      require(r.statistics[2] == 1 && std::isfinite(r.irradiance[0][3]), "Depth capture preserves explicit one-ray hit metadata");
      if (miss) ++misses;
      else { ++hits; require(std::abs(r.irradiance[0][3] - 1) < 1e-4f, "Roof depth is captured from the light-facing plane"); }
      depth[destinations[i]] = miss ? 8 : r.irradiance[0][3];
    }
    rays.clear(); destinations.clear();
  };
  for (uint32_t slot = 0; slot < layout.tiles.size(); ++slot) {
    const auto allocation = layout.allocation(slot);
    for (uint32_t y = 0; y < extent; ++y) for (uint32_t x = 0; x < extent; ++x) {
      bake::Point ray;
      ray.position = {layout.tiles[slot][0] + (float(x) - options.border + .5f) * texel,
                      layout.tiles[slot][1] + (float(y) - options.border + .5f) * texel, 2, 0};
      ray.normal = {0,0,-1,0};
      rays.push_back(ray);
      destinations.push_back((allocation.y - options.border + y) * options.pageSize + allocation.x - options.border + x);
      if (rays.size() == 256) flush();
    }
  }
  flush();
  require(hits && misses, "Sparse depth retains both roof occlusion and the narrow opening");
  std::vector<bake::Point> receivers;
  for (float z : {0.f,1.5f}) for (float x : {-.125f,-.025f,.025f,.125f})
    receivers.push_back({{x,.425f,z,0},{0,0,1,0}});
  fixture.begin();
  transport.recordSun(fixture.command, 0, receivers, 64);
  fixture.finish();
  std::vector<bake::Result> reference(receivers.size());
  transport.read(0, reference);
  for (size_t i = 0; i < receivers.size(); ++i) {
    const auto &p = receivers[i].position;
    const int32_t tx = int32_t(std::floor(p[0])), ty = int32_t(std::floor(p[1]));
    const auto word = layout.directory[size_t(ty - layout.minimum[1]) * layout.dimensions[0] + tx - layout.minimum[0]];
    require(word != 0, "Prototype receivers are inside complete virtual coverage");
    const auto allocation = layout.allocation(word-1);
    const float qx = (p[0] - tx) / texel - .5f, qy = (p[1] - ty) / texel - .5f;
    const int32_t x0 = int32_t(std::floor(qx)), y0 = int32_t(std::floor(qy));
    float visible = 0;
    for (int32_t y = 0; y < 2; ++y) for (int32_t x = 0; x < 2; ++x) {
      const float wx = x ? qx-x0 : 1-(qx-x0), wy = y ? qy-y0 : 1-(qy-y0);
      const auto index = (int32_t(allocation.y)+y0+y) * int32_t(options.pageSize) + int32_t(allocation.x)+x0+x;
      visible += wx * wy * (2-p[2] <= depth.at(size_t(index)) + .0001f ? 1.f : 0.f);
    }
    const float expected = p[2] > 1 || std::abs(p[0]) < .075f ? 1.f : 0.f;
    require(std::abs(reference[i].irradiance[0][0]-expected) < 1e-5f && std::abs(visible-expected) < 1e-4f,
            "Sparse depth matches direct sun queries across a sub-probe opening and above/below the roof");
  }
  std::printf("Sparse sun depth prototype: %u hits, %u misses, %zu receiver comparisons; %llu image bytes\n",
              hits, misses, receivers.size(), static_cast<unsigned long long>(layout.depthBytes));
}
