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
  transportOptions.depthCapture = true;
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
  Buffer atlasDepth;
  atlasDepth.create(fixture.physical, fixture.device, layout.depthBytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Device);
  fixture.begin();
  vkCmdFillBuffer(fixture.command, atlasDepth.handle, 0, layout.depthBytes, 0xbf800000u);
  const VkMemoryBarrier initialAtlas{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0, 1, &initialAtlas, 0, nullptr, 0, nullptr);
  fixture.finish();
  std::vector<bake::Point> rays;
  std::vector<uint32_t> destinations;
  Buffer compactDepth, compactReadback;
  constexpr size_t compactBytes = (256 + 2) * sizeof(float);
  compactDepth.create(fixture.physical, fixture.device, compactBytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      Buffer::Memory::Device);
  compactReadback.create(fixture.physical, fixture.device, compactBytes,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
  uint32_t hits = 0, misses = 0;
  const bake::Point validDepthRay{{0, 0, 2, 0}, {0, 0, -1, 0}};
  for (uint32_t firstWord : {uint32_t(258), UINT32_MAX}) {
    bool rejected = false;
    try { transport.recordDepth(fixture.command, 0, {&validDepthRay, 1}, compactDepth.descriptor(), firstWord); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "Compact depth rejects destination overflow before recording GPU work");
  }
  VkPhysicalDeviceProperties depthProperties;
  vkGetPhysicalDeviceProperties(fixture.physical, &depthProperties);
  if (depthProperties.limits.minStorageBufferOffsetAlignment > 1) {
    auto unaligned = compactDepth.descriptor();
    unaligned.offset = 1;
    bool alignmentRejected = false;
    try { transport.recordDepth(fixture.command, 0, {&validDepthRay, 1}, unaligned); }
    catch (const std::invalid_argument &) { alignmentRejected = true; }
    require(alignmentRejected, "Compact depth rejects misaligned storage descriptors");
  }
  uint32_t depthBatch = 0;
  auto flush = [&] {
    if (rays.empty()) return;
    const uint32_t slot = depthBatch++ % gpuFrameCount;
    fixture.begin();
    transport.recordGeometry(fixture.command, slot, rays);
    fixture.finish();
    std::vector<bake::Result> results(rays.size());
    transport.read(slot, results);
    fixture.begin();
    vkCmdFillBuffer(fixture.command, compactDepth.handle, 0, compactBytes, 0x7fc00000u);
    const VkMemoryBarrier fillBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &fillBarrier, 0, nullptr, 0, nullptr);
    transport.recordDepth(fixture.command, slot, rays, compactDepth.descriptor(), 1);
    const VkBufferCopy copy{0, 0, compactBytes};
    vkCmdCopyBuffer(fixture.command, compactDepth.handle, compactReadback.handle, 1, &copy);
    const VkMemoryBarrier hostBarrier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &hostBarrier, 0, nullptr, 0, nullptr);
    fixture.finish();
    bake::Result unavailable;
    bool readRejected = false;
    try { transport.read(slot, {&unavailable, 1}); }
    catch (const std::invalid_argument &) { readRejected = true; }
    require(readRejected, "Caller-owned depth cannot be mistaken for ordinary transport readback");
    std::array<float, 258> compact;
    compactReadback.read(compact.data(), compactBytes);
    require(std::isnan(compact[0]) && std::isnan(compact[rays.size() + 1]),
            "Compact depth writes only its explicit destination range");
    for (size_t i = 0; i < results.size(); ++i) {
      const auto &r = results[i];
      const bool miss = r.statistics[1] > .5f;
      require(miss ? compact[i + 1] < 0 : std::abs(compact[i + 1] - r.irradiance[0][3]) < 1e-5f,
              "Four-byte GPU depth agrees with full geometry readback and explicit miss metadata");
      require(r.statistics[2] == 1 && std::isfinite(r.irradiance[0][3]), "Depth capture preserves explicit one-ray hit metadata");
      if (miss) ++misses;
      else { ++hits; require(std::abs(r.irradiance[0][3] - 1) < 1e-4f, "Roof depth is captured from the light-facing plane"); }
      depth[destinations[i]] = compact[i + 1] < 0 ? 8 : compact[i + 1];
    }
    std::vector<bake::Transport::DepthRay> indexed;
    for (size_t i = 0; i < rays.size(); ++i)
      indexed.push_back({{rays[i].position[0],rays[i].position[1],rays[i].position[2]},
                         {rays[i].normal[0],rays[i].normal[1],rays[i].normal[2]}, destinations[i]});
    fixture.begin();
    transport.recordIndexedDepth(fixture.command, slot, indexed, atlasDepth.descriptor(), 8);
    fixture.finish();
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
      if (rays.size() == 127) flush();
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
  const auto gpuVisibility = querySunDepth(fixture, layout, atlasDepth, receivers);
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
    require(gpuVisibility[i][1] == 1 && std::abs(gpuVisibility[i][0] - expected) < 1e-4f,
            "Shared GPU sparse-depth lookup agrees with captured depth and independent sun visibility");
    require(std::abs(reference[i].irradiance[0][0]-expected) < 1e-5f && std::abs(visible-expected) < 1e-4f,
            "Sparse depth matches direct sun queries across a sub-probe opening and above/below the roof");
  }
  const auto negativeTile = layout.directory[2] - 1;
  const auto edgeAllocation = layout.allocation(negativeTile);
  bool filteredEdge = false;
  for (uint32_t x = 0; x + 1 < options.tileTexels && !filteredEdge; ++x) {
    const auto address = (edgeAllocation.y + 8) * options.pageSize + edgeAllocation.x + x;
    if ((depth[address] > 2) == (depth[address + 1] > 2)) continue;
    const bake::Point midpoint{{-1 + float(x + 1) * texel, .425f, 0, 0}, {}};
    const auto filtered = querySunDepth(fixture, layout, atlasDepth, {&midpoint, 1});
    require(filtered[0][1] == 1 && std::abs(filtered[0][0] - .5f) < 1e-4f,
            "GPU filtering blends visibility comparisons rather than averaging foreground/background depth");
    filteredEdge = true;
  }
  require(filteredEdge, "The narrow-opening fixture contains a captured filtering boundary");
  auto rotatedReceivers = receivers;
  for (auto &receiver : rotatedReceivers) {
    const float x = receiver.position[0];
    receiver.position[0] = -receiver.position[1];
    receiver.position[1] = x;
  }
  const auto rotated = querySunDepth(fixture, layout, atlasDepth, rotatedReceivers,
                                     {Vec3{0,1,0},Vec3{-1,0,0},Vec3{0,0,-1}});
  for (size_t i = 0; i < rotated.size(); ++i)
    require(rotated[i] == gpuVisibility[i], "GPU lookup applies the supplied sun basis independently of world axes");
  auto missingTile = layout;
  missingTile.directory.back() = 0;
  const auto hole = querySunDepth(fixture, missingTile, atlasDepth, {receivers.data()+3, 1});
  require(hole[0][0] == -1 && hole[0][1] == 0, "A sparse directory hole requests fallback inside the outer coverage bounds");
  const auto emptyLayout = bake::planSunDepthLayout({}, options);
  const auto disabled = querySunDepth(fixture, *emptyLayout, atlasDepth, {receivers.data(), 1});
  require(disabled[0][0] == -1 && disabled[0][1] == 0, "An unpublished empty map performs no depth sampling");
  const std::array<bake::Point, 2> uncovered{{{{3, 0, 0, 0}, {}}, {{0, 0, 3, 0}, {}}}};
  const auto fallback = querySunDepth(fixture, layout, atlasDepth, uncovered);
  for (const auto &value : fallback)
    require(value[0] == -1 && value[1] == 0, "Missing XY or depth coverage requests fallback instead of leaking sunlight");
  const bake::Transport::DepthRay distantRay{{.125f,.425f,10}, {0,0,-1}, 0};
  for (float range : {.5f,12.f}) {
    fixture.begin();
    transport.recordIndexedDepth(fixture.command, 0, {&distantRay, 1}, compactDepth.descriptor(), range);
    const VkBufferCopy copy{0,0,sizeof(float)};
    vkCmdCopyBuffer(fixture.command, compactDepth.handle, compactReadback.handle, 1, &copy);
    const VkMemoryBarrier host{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &host, 0, nullptr, 0, nullptr);
    fixture.finish();
    float value;
    compactReadback.read(&value, sizeof(value));
    require(range < 9 ? value < 0 : std::abs(value - 9) < 1e-4f,
            "Explicit depth range reaches casters beyond the default transport cap and preserves bounded misses");
  }
  fixture.begin();
  vkCmdFillBuffer(fixture.command, atlasDepth.handle, 0, layout.depthBytes, 0xc0000000u); // -2
  const VkMemoryBarrier unresolved{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
  vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0,1,&unresolved,0,nullptr,0,nullptr);
  fixture.finish();
  const auto transmissionFallback = querySunDepth(fixture, layout, atlasDepth, receivers);
  for (const auto &value : transmissionFallback)
    require(value[1] == 0, "Unresolved transmission requests fallback rather than fully lit or fully shadowed lookup");
  std::printf("Sparse sun depth prototype: %u hits, %u misses, %zu receiver comparisons; %llu image bytes\n",
              hits, misses, receivers.size(), static_cast<unsigned long long>(layout.depthBytes));
}
