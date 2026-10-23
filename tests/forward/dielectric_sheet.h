#pragma once
// Included after World in bake_transport.cpp; shares its Vulkan resource fixture.
  void dielectricSheetTransport(test::Fixture &fixture, pipeline::Service &pipelines, World &world) {
    remixapi_MaterialInfoTranslucentEXT sheet{};
    sheet.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    sheet.refractiveIndex = 1.5f;
    sheet.transmittanceColor = {.5f, .5f, .5f};
    sheet.transmittanceMeasurementDistance = 1;
    sheet.thinWallThickness_hasvalue = 1;
    sheet.thinWallThickness_value = .25f;
    remixapi_MaterialBakeBoundaryEXT boundary{};
    boundary.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_BOUNDARY_EXT;
    boundary.pNext = &sheet;
    boundary.smoothBoundary = 1;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &boundary;
    info.hash = ++world.hash;
    remixapi_MaterialHandle material;
    world.resources.createMaterial(info, material);
    const auto plainSheetMaterial = material;
    const auto panel = world.quad(material,
        {{{-100, -100, 1}, {100, -100, 1}, {100, 100, 1}, {-100, 100, 1}}},
        nullptr, 0xffffffff, {0, 0, 1});
    auto scene = world.scene({panel});
    scene->dome.enabled = true;
    scene->dome.radiance = {1, 1, 1};
    scene::SphereLight lamp{};
    lamp.id = ++world.hash;
    lamp.position[2] = 5;
    lamp.radius = .01f;
    std::fill_n(lamp.radiance, 3, 1.f);
    scene->lights.push_back(lamp);
    bake::Transport transport;
    bake::Options options;
    options.batchPoints = 8;
    options.bias = .0001f;
    auto prepare = [&](bake::Transport &target, std::shared_ptr<scene::BakeScene> source, const char *name) {
      auto job = pipelines.prepare(name, [&, source] {
        target.initialize(fixture.physical, fixture.device, source, options);
      });
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      while (!job->ready() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      require(job->ready(), "Sheet transport preparation completed");
      job->check();
      fixture.begin();
      target.recordPrepare(fixture.command, world.resources);
      fixture.finish();
    };
    prepare(transport, scene, "thin-dielectric-transport");
    const bake::VisibilityPoint points[] = {{{{0, 0, -2, 0}, {0, 0, 1, 0}}, 0},
                                            {{{0, 0, -20, 0}, {0, 0, 1, 0}}, 0},
                                            {{{0, 0, 2, 0}, {0, 0, 1, 0}}, 0}};
    fixture.begin();
    transport.recordVisibility(fixture.command, 0, points, 1);
    fixture.finish();
    bake::VisibilityResult visibility[3];
    transport.readVisibility(0, visibility);
    const float f = .04f, absorbed = std::pow(.5f, .25f);
    const float denominator = 1 - f * f * absorbed * absorbed;
    const float transmitted = (1 - f) * (1 - f) * absorbed / denominator;
    const float reflected = f + (1 - f) * (1 - f) * f * absorbed * absorbed / denominator;
    std::printf("Sheet visibility observed %.6f %.6f %.6f, expected %.6f\n",
                visibility[0].visibility, visibility[1].visibility, visibility[2].visibility, transmitted);
    require(std::abs(visibility[0].visibility - transmitted) < 2e-4f &&
                std::abs(visibility[1].visibility - transmitted) < 2e-4f &&
                std::abs(visibility[2].visibility - 1) < 2e-4f,
            "A sheet applies two-interface transmission once, independently of surrounding distance");
    const bake::Point rays[] = {{{0, 0, -2, 0}, {0, 0, 1, 0}},
                                {{0, 0, -20, 0}, {0, 0, 1, 0}},
                                {{0, 0, 2, 0}, {0, 0, -1, 0}}};
    fixture.begin();
    transport.recordRadiance(fixture.command, 0, rays, 1);
    fixture.finish();
    bake::Result results[3];
    transport.read(0, results);
    for (const auto &result : results) {
      require(result.statistics[0] == 0, "Neither side of a sheet is an invalid containing solid");
      for (uint32_t c = 0; c < 3; ++c)
        require(std::abs(result.irradiance[0][c] - (reflected + transmitted)) < 3e-4f,
                "Both sheet sides preserve finite reflection/transmission energy without an eta-squared gain");
    }
    std::printf("Measured sheet: visibility %.6f/%.6f, energy %.6f\n",
                visibility[0].visibility, transmitted, results[0].irradiance[0][0]);
    const uint8_t tintPixels[] = {64, 64, 64, 255, 192, 192, 192, 255};
    world.texture(0x7e11, tintPixels);
    sheet.transmittanceTexture = L"0x7e11";
    info.hash = ++world.hash;
    world.resources.createMaterial(info, material);
    const auto tintedPanel = world.quad(material,
        {{{-100, -100, 1}, {100, -100, 1}, {100, 100, 1}, {-100, 100, 1}}},
        nullptr, 0xffffffff, {0, 0, 1});
    auto tintedScene = world.scene({tintedPanel});
    tintedScene->dome = scene->dome;
    bake::Transport tinted;
    prepare(tinted, tintedScene, "textured-thin-dielectric");
    const bake::Point tintRays[] = {{{-50, 0, -2, 0}, {0, 0, 1, 0}},
                                    {{50, 0, 2, 0}, {0, 0, -1, 0}}};
    fixture.begin();
    tinted.recordRadiance(fixture.command, 0, tintRays, 1);
    fixture.finish();
    tinted.read(0, {results, 2});
    for (uint32_t pixel = 0; pixel < 2; ++pixel) {
      const float a = std::pow(.5f * tintPixels[pixel * 4] / 255.f, .25f);
      const float d = 1 - f * f * a * a;
      const float expected = f + (1 - f) * (1 - f) * (a + f * a * a) / d;
      require(std::abs(results[pixel].irradiance[0][0] - expected) < 3e-4f,
              "Sheet UVs modulate measured absorption on either side of a tinted pane");
    }
    auto scrollingScene = std::make_shared<scene::BakeScene>(*tintedScene);
    scrollingScene->draws[0].material = std::make_shared<scene::Material>(*tintedScene->draws[0].material);
    scrollingScene->draws[0].material->transmittanceScroll[0] = .5f;
    scrollingScene->draws[0].material->wrapU = 1;
    bake::Transport scrolling;
    prepare(scrolling, scrollingScene, "scrolling-thin-dielectric");
    for (uint32_t step = 0; step < 3; ++step) {
      fixture.begin();
      scrolling.updateLighting(0, {}, scrollingScene->sun, scrollingScene->dome, 1000, float(step));
      scrolling.recordRadiance(fixture.command, 0, tintRays, 1);
      fixture.finish();
      bake::Result animated[2];
      scrolling.read(0, animated);
      for (uint32_t pixel = 0; pixel < 2; ++pixel)
        require(std::abs(animated[pixel].irradiance[0][0] - results[(pixel + step) % 2].irradiance[0][0]) < 3e-4f,
                "A streamed tint clock shifts and wraps sheet absorption without rebuilding geometry");
    }
    for (uint32_t axes : {1u, 2u, 3u}) {
      auto averagedScene = std::make_shared<scene::BakeScene>(*tintedScene);
      auto averagedMaterial = std::make_shared<scene::Material>(*tintedScene->draws[0].material);
      averagedScene->draws[0].material = averagedMaterial;
      averagedMaterial->bakeTintAxes = axes;
      averagedMaterial->transmittanceScroll[0] = axes & 1u ? .5f : 0;
      averagedMaterial->transmittanceScroll[1] = axes & 2u ? .25f : 0;
      bake::Transport averaged;
      prepare(averaged, averagedScene, ("averaged-tint-" + std::to_string(axes)).c_str());
      std::array<bake::Result, 2> firstMean;
      for (uint32_t phase = 0; phase < 2; ++phase) {
        fixture.begin();
        averaged.updateLighting(0, {}, averagedScene->sun, averagedScene->dome, 1000, float(phase * 5));
        averaged.recordRadiance(fixture.command, 0, tintRays, 1, 8192);
        fixture.finish();
        std::array<bake::Result, 2> mean;
        averaged.read(0, mean);
        for (uint32_t pixel = 0; pixel < 2; ++pixel) {
          const float expected = axes & 1u
              ? .5f * (results[0].irradiance[0][0] + results[1].irradiance[0][0])
              : results[pixel].irradiance[0][0];
          require(std::abs(mean[pixel].irradiance[0][0] - expected) < .003f,
                  "Averaging samples sheet response while unselected axes retain spatial tint");
        }
        if (phase == 0) firstMean = mean;
        else require(std::memcmp(firstMean.data(), mean.data(), sizeof(mean)) == 0,
                     "The explicit stationary tint distribution is independent of live animation time");
      }
    }
    // A vertical texture makes a missing V-phase implementation observable;
    // the horizontal fixture above separately checks preserving unselected U.
    remixapi_TextureInfo vertical{};
    vertical.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    vertical.hash = 0x7e12;
    vertical.width = 1;
    vertical.height = 2;
    vertical.depth = vertical.mipLevels = 1;
    vertical.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
    vertical.data = tintPixels;
    vertical.dataSize = sizeof(tintPixels);
    remixapi_TextureHandle verticalImage;
    world.resources.createTexture(vertical, verticalImage);
    sheet.transmittanceTexture = L"0x7e12";
    info.hash = ++world.hash;
    world.resources.createMaterial(info, material);
    const auto verticalPanel = world.quad(material,
        {{{-100, -100, 1}, {100, -100, 1}, {100, 100, 1}, {-100, 100, 1}}},
        nullptr, 0xffffffff, {0, 0, 1});
    auto verticalScene = world.scene({verticalPanel});
    verticalScene->dome = scene->dome;
    verticalScene->draws[0].material = std::make_shared<scene::Material>(*verticalScene->draws[0].material);
    verticalScene->draws[0].material->bakeTintAxes = 2;
    verticalScene->draws[0].material->transmittanceScroll[1] = .5f;
    bake::Transport verticalMean;
    prepare(verticalMean, verticalScene, "vertical-tint-average");
    const bake::Point verticalRays[] = {{{0, -50, -2, 0}, {0, 0, 1, 0}},
                                       {{0, 50, -2, 0}, {0, 0, 1, 0}}};
    fixture.begin();
    verticalMean.recordRadiance(fixture.command, 0, verticalRays, 1, 8192);
    fixture.finish();
    std::array<bake::Result, 2> verticalResults;
    verticalMean.read(0, verticalResults);
    const float expectedMean = .5f * (results[0].irradiance[0][0] + results[1].irradiance[0][0]);
    for (const auto &result : verticalResults)
      require(std::abs(result.irradiance[0][0] - expectedMean) < .003f,
              "The V averaging axis integrates vertical tint variation from either receiver position");
    // Diagonal dark texels distinguish independent U/V phases from reusing
    // one random phase for both coordinates (which would stay on the diagonal).
    const uint8_t checkerPixels[] = {64, 64, 64, 255, 192, 192, 192, 255,
                                    192, 192, 192, 255, 64, 64, 64, 255};
    vertical.hash = 0x7e13;
    vertical.width = vertical.height = 2;
    vertical.data = checkerPixels;
    vertical.dataSize = sizeof(checkerPixels);
    world.resources.createTexture(vertical, verticalImage);
    sheet.transmittanceTexture = L"0x7e13";
    info.hash = ++world.hash;
    world.resources.createMaterial(info, material);
    const auto checkerPanel = world.quad(material,
        {{{-100, -100, 1}, {100, -100, 1}, {100, 100, 1}, {-100, 100, 1}}},
        nullptr, 0xffffffff, {0, 0, 1});
    auto checkerScene = world.scene({checkerPanel});
    checkerScene->dome = scene->dome;
    checkerScene->draws[0].material = std::make_shared<scene::Material>(*checkerScene->draws[0].material);
    checkerScene->draws[0].material->bakeTintAxes = 3;
    bake::Transport checkerMean;
    prepare(checkerMean, checkerScene, "independent-tint-phases");
    fixture.begin();
    checkerMean.recordRadiance(fixture.command, 0, verticalRays, 1, 8192);
    fixture.finish();
    checkerMean.read(0, verticalResults);
    for (const auto &result : verticalResults)
      require(std::abs(result.irradiance[0][0] - expectedMean) < .003f,
              "Combined tint averaging samples the joint UV distribution rather than a correlated diagonal");
    // Surround the point with sheets on all six axes. They must not hide the
    // containing volume's exits during the initial-medium lookup.
    auto body = sheet;
    body.thinWallThickness_hasvalue = 0;
    body.thinWallThickness_value = 0;
    body.refractiveIndex = 1;
    body.transmittanceColor = {.9f, .9f, .9f};
    body.transmittanceTexture = nullptr;
    boundary.pNext = &body;
    info.hash = ++world.hash;
    world.resources.createMaterial(info, material);
    const auto upper = world.quad(material,
        {{{-1000, -1000, 10}, {1000, -1000, 10}, {1000, 1000, 10}, {-1000, 1000, 10}}},
        nullptr, 0xffffffff, {0, 0, 1});
    const auto lower = world.quad(material,
        {{{-1000, -1000, -10}, {1000, -1000, -10}, {1000, 1000, -10}, {-1000, 1000, -10}}},
        nullptr, 0xffffffff, {0, 0, -1});
    std::array<uint64_t, 6> panes;
    for (uint32_t axis = 0; axis < 3; ++axis)
      for (uint32_t side = 0; side < 2; ++side) {
        Vec3 n{};
        n[axis] = side ? 1.f : -1.f;
        std::array<Vec3, 4> corners{};
        constexpr float u[] = {-1, 1, 1, -1}, v[] = {-1, -1, 1, 1};
        for (uint32_t corner = 0; corner < 4; ++corner) {
          corners[corner][axis] = n[axis];
          corners[corner][(axis + 1) % 3] = u[corner];
          corners[corner][(axis + 2) % 3] = v[corner];
        }
        panes[axis * 2 + side] = world.quad(plainSheetMaterial, corners, nullptr, 0xffffffff, n);
      }
    auto mixedScene = world.scene({upper, lower, panes[0], panes[1], panes[2], panes[3], panes[4], panes[5]});
    lamp.position[2] = 20;
    mixedScene->lights.push_back(lamp);
    bake::Transport mixed;
    prepare(mixed, mixedScene, "sheet-inside-measured-volume");
    const bake::VisibilityPoint mixedPoints[] = {{{{0, 0, 0, 0}, {0, 0, 1, 0}}, 0},
                                                 {{{0, 0, 2, 0}, {0, 0, 1, 0}}, 0},
                                                 {{{0, 0, 12, 0}, {0, 0, 1, 0}}, 0}};
    fixture.begin();
    mixed.recordVisibility(fixture.command, 0, mixedPoints, 1);
    fixture.finish();
    mixed.readVisibility(0, visibility);
    require(std::abs(visibility[0].visibility - transmitted * std::pow(.9f, 10)) < 2e-4f &&
                std::abs(visibility[1].visibility - std::pow(.9f, 8)) < 2e-4f &&
                std::abs(visibility[2].visibility - 1) < 2e-4f,
            "Thin panes cannot hide a containing medium or create absorption outside it");
  }
