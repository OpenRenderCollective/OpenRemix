// A first-hit depth cannot encode coloured transmission or proportional coverage.
void sunDepthTransmission(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
  remixapi_MaterialInfoTranslucentEXT glass{};
  glass.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
  glass.refractiveIndex = 1.5f;
  glass.transmittanceColor = {.5f,.75f,1};
  glass.transmittanceMeasurementDistance = 1;
  remixapi_MaterialBakeBoundaryEXT boundary{};
  boundary.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_BOUNDARY_EXT;
  boundary.pNext = &glass;
  boundary.smoothBoundary = 1;
  remixapi_MaterialInfo info{};
  info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  info.pNext = &boundary;
  info.hash = ++world.hash;
  remixapi_MaterialHandle dielectric;
  world.resources.createMaterial(info, dielectric);
  remixapi_InstanceInfoBlendEXT blend{};
  blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  blend.alphaBlendEnabled = 1;
  blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6;
  blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7;
  blend.writeMask = 15;
  const auto glassWall = world.wall(dielectric, 1, 2);
  const auto halfWall = world.wall(world.material({.opacity=.5f}), 1, 2, &blend);
  const auto emptyWall = world.wall(world.material({.opacity=0}), 1, 2, &blend);
  const auto cutoutWall = world.wall(world.material({.opacity=0, .cutout=true}), 1, 2);
  const auto solidWall = world.wall(world.material({}), 1, 2);
  const auto fullCoverage = world.wall(world.material({.opacity=1}), 1, 2, &blend);
  const uint64_t instances[]{glassWall,halfWall,emptyWall,cutoutWall,solidWall,fullCoverage};
  const float expected[]{-2,-2,-1,-1,1,1};
  Buffer output, readback;
  output.create(fixture.physical, fixture.device, sizeof(float),
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Memory::Device);
  readback.create(fixture.physical, fixture.device, sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
  for (uint32_t i = 0; i < std::size(instances); ++i) {
    bake::Transport transport;
    bake::Options options;
    options.depthCapture = true;
    options.batchPoints = 1;
    auto scene = world.scene({instances[i]});
    auto preparation = pipelines.prepare("sun-depth-material-" + std::to_string(i), [&] {
      transport.initialize(fixture.physical, fixture.device, scene, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!preparation->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(preparation->ready(), "Material-aware depth preparation completed");
    preparation->check();
    const bake::Transport::DepthRay ray{{.125f,.425f,2},{0,0,-1},0};
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    transport.recordIndexedDepth(fixture.command, 0, {&ray,1}, output.descriptor(), 4);
    const VkBufferCopy copy{0,0,sizeof(float)};
    vkCmdCopyBuffer(fixture.command, output.handle, readback.handle, 1, &copy);
    const VkMemoryBarrier host{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(fixture.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&host,0,nullptr,0,nullptr);
    fixture.finish();
    transport.releaseBuildInputs();
    float depth;
    readback.read(&depth,sizeof(depth));
    require(std::abs(depth-expected[i])<1e-4f,
            "Depth distinguishes transmissive fallback, empty coverage and opaque first hits");
  }
  std::puts("Sun depth materials: glass and partial coverage request fallback; empty and opaque remain exact");
}
