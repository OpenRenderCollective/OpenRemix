// Private transfer-field prototype. Geometry is captured on the GPU; reference
// reconstruction stays on the CPU until the shader/storage contract is tested.
auto coarseTransferPrototype(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
  const std::array<Vec3, 3> coarse{{{-1,0,2}, {1,0,0}, {0,0,2}}};
  const std::array<Vec3, 2> fine{{{-1,0,0}, {0,0,0}}};
  constexpr float epsilon = .0001f;
  std::vector<bake::Transport::DepthRay> rays;
  std::vector<float> lengths;
  for (const auto &origin : coarse) for (const auto &target : fine) {
    Vec3 delta;
    for (uint32_t c = 0; c < 3; ++c) delta[c] = target[c] - origin[c];
    const float length = std::sqrt(dot(delta, delta));
    const auto direction = normalized(delta);
    Vec3 start;
    for (uint32_t c = 0; c < 3; ++c) start[c] = origin[c] + direction[c] * epsilon;
    rays.push_back({start, direction, uint32_t(rays.size())});
    lengths.push_back(length - 2 * epsilon);
  }
  // A hit beyond the fine endpoint must not occlude the finite connection.
  rays.push_back({{-1,0,2-epsilon}, {0,0,-1}, uint32_t(rays.size())});
  lengths.push_back(.5f-2*epsilon);
  Buffer output, readback;
  const auto bytes = rays.size() * sizeof(float);
  output.create(fixture.physical, fixture.device, bytes,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, Buffer::Memory::Device);
  readback.create(fixture.physical, fixture.device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, Buffer::Memory::Readback);
  auto capture = [&](remixapi_MaterialHandle material, const char *name,
                     remixapi_InstanceInfoBlendEXT *blend = nullptr) {
    const auto left = world.quad(material, {{{-2,-2,1},{-.2f,-2,1},{-.2f,2,1},{-2,2,1}}}, blend);
    const auto right = world.quad(material, {{{.2f,-2,1},{2,-2,1},{2,2,1},{.2f,2,1}}}, blend);
    auto scene = world.scene({left, right});
    require(scene->lights.empty(), "Transfer geometry has no per-light input");
    bake::Options options;
    options.depthCapture = true;
    options.batchPoints = uint32_t(rays.size());
    bake::Transport transport;
    auto preparation = pipelines.prepare(name, [&] {
      transport.initialize(fixture.physical, fixture.device, scene, options);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!preparation->ready() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(preparation->ready(), "Coarse transfer geometry preparation completed");
    preparation->check();
    fixture.begin();
    transport.recordPrepare(fixture.command, world.resources);
    transport.recordIndexedDepth(fixture.command, 0, rays, output.descriptor(), 8);
    const VkBufferCopy copy{0,0,bytes};
    vkCmdCopyBuffer(fixture.command, output.handle, readback.handle, 1, &copy);
    const VkMemoryBarrier host{.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(fixture.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0,1,&host,0,nullptr,0,nullptr);
    fixture.finish();
    transport.releaseBuildInputs();
    std::array<float, 7> depths;
    readback.read(depths.data(), bytes);
    return depths;
  };
  const auto opaque = capture(world.material({}), "coarse-transfer-opaque");
  // Unknown is retained separately from clear: both use ordinary fallback, but
  // only clear is evidence supporting an occlusion decision elsewhere.
  auto classify = [&](const auto &depths) {
    std::array<uint8_t, 7> visibility{}, known{};
    for (uint32_t i = 0; i < depths.size(); ++i) {
      require(std::isfinite(depths[i]) && (depths[i] >= 0 || depths[i] == -1 || depths[i] == -2),
              "Transfer capture has a finite distance or an explicit sentinel");
      known[i] = depths[i] != -2;
      visibility[i] = !known[i] || depths[i] == -1 || depths[i] >= lengths[i] ? 255 : 0;
    }
    return std::pair{visibility, known};
  };
  const auto [visibility, known] = classify(opaque);
  require(known == std::array<uint8_t,7>{1,1,1,1,1,1,1} &&
              visibility == std::array<uint8_t,7>{0,0,255,255,0,255,255},
          "Transfer capture separates blocked segments, same-room donors and the opening");
  auto reconstruct = [](const auto &field, float fineShare, float coarseBright, float coarseDark, float fineValue) {
    const std::array<float, 2> coarseWeights{.75f,.25f}, values{coarseBright,coarseDark};
    float sum = fineShare * fineValue, total = fineShare;
    for (uint32_t donor = 0; donor < 2; ++donor) {
      const float visible = (field[donor*2] + field[donor*2+1]) / 510.f;
      const float weight = (1-fineShare) * coarseWeights[donor] * (1-fineShare + fineShare*visible);
      sum += weight * values[donor]; total += weight;
    }
    return sum / total;
  };
  require(reconstruct(visibility,.8f,1.5f,.1f,.075f) < .14f &&
              std::abs(reconstruct(visibility,0,1.5f,.1f,.075f)-1.15f) < 1e-6f &&
              std::abs(reconstruct(visibility,1,1.5f,.1f,.075f)-.075f) < 1e-6f,
          "Captured coarse correction reduces leakage and preserves transition endpoints");
  for (uint32_t i = 0; i <= 100; ++i) {
    const float f = i / 100.f;
    require(std::abs(reconstruct(visibility,f,.7f,.7f,.7f)-.7f) < 1e-6f &&
                reconstruct(visibility,f,0,0,0) == 0,
            "Normalized transfer reconstruction preserves constant energy and valid black");
    const float a = reconstruct(visibility,f,.3f,.1f,.04f);
    const float b = reconstruct(visibility,f,.7f,.2f,.06f);
    require(std::abs(a+b-reconstruct(visibility,f,1.f,.3f,.1f)) < 1e-6f,
            "The same geometry weights preserve additive controllable-light components");
  }
  const auto [cutout, cutoutKnown] = classify(capture(world.material({.opacity=0, .cutout=true}),
                                                     "coarse-transfer-cutout"));
  require(cutout == std::array<uint8_t,7>{255,255,255,255,255,255,255} &&
              cutoutKnown == std::array<uint8_t,7>{1,1,1,1,1,1,1},
          "Empty cutout coverage cannot become an opaque transfer blocker");
  remixapi_InstanceInfoBlendEXT partial{};
  partial.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
  partial.alphaBlendEnabled = 1;
  partial.srcColorBlendFactor = partial.srcAlphaBlendFactor = 6;
  partial.dstColorBlendFactor = partial.dstAlphaBlendFactor = 7;
  partial.writeMask = 15;
  const auto [partialVisibility, partialKnown] = classify(capture(world.material({.opacity=.5f}),
                                                                "coarse-transfer-partial", &partial));
  require(partialVisibility == std::array<uint8_t,7>{255,255,255,255,255,255,255} &&
              partialKnown == std::array<uint8_t,7>{0,0,1,1,0,1,0},
          "Proportional coverage requests conservative transfer fallback");
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
  const auto [fallback, resolved] = classify(capture(dielectric, "coarse-transfer-transmission"));
  require(resolved == std::array<uint8_t,7>{0,0,1,1,0,1,0} &&
              fallback == std::array<uint8_t,7>{255,255,255,255,255,255,255},
          "Transmissive transfer connections retain explicit unknown fallback");
  require(std::abs(reconstruct(fallback,.8f,1.5f,.1f,.075f)-.29f) < 1e-6f,
          "Unknown transfer preserves the ordinary blended estimate");
  std::puts("Coarse transfer prototype: opaque/clear/opening/unknown capture and normalized CPU reconstruction pass");
  return std::pair{visibility, known};
}
