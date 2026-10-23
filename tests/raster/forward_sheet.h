#pragma once
// Included inside the rendering fixture namespace after Target and helpers.
  void sheetTests() {
    for (uint32_t samples : {1u, 2u, 4u}) {
      Target target;
      auto &resources = target.raster;
      remix::forward::Options options;
      options.water = true;
      options.samples = samples;
      options.ambient = 1;
      options.shadows = options.localShadows = options.ssr = false;
      options.baking.enabled = false;
      options.baking.reflectionProbes = 0;
      target.enableForward(options);
      const auto background = quad(resources, 0xe300, material(resources, 0xe301, 1, 1, 1));
      remixapi_MaterialInfoTranslucentEXT optical{};
      optical.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
      optical.refractiveIndex = 1;
      optical.transmittanceColor = {.0625f, .25f, 1};
      optical.transmittanceMeasurementDistance = 1;
      optical.thinWallThickness_hasvalue = 1;
      optical.thinWallThickness_value = .25f;
      remixapi_MaterialInfo info{};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.hash = 0xe302;
      info.pNext = &optical;
      remixapi_MaterialHandle measured;
      resources.createMaterial(info, measured);
      const auto pane = quad(resources, 0xe303, measured);
      const auto foreground = quad(resources, 0xe304, material(resources, 0xe305, 0, 0, 1));
      const uint8_t texel[] = {64, 128, 255, 255};
      remixapi_TextureInfo texture{};
      texture.hash = 0xe306;
      texture.width = texture.height = 1;
      texture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
      texture.data = texel;
      texture.dataSize = sizeof(texel);
      remixapi_TextureHandle image;
      resources.createTexture(texture, image);
      info.hash = 0xe307;
#if defined(_WIN32)
      info.albedoTexture = L"0xe306";
#else
      info.albedoTexture = "0xe306";
#endif
      resources.createMaterial(info, measured);
      const auto texturedPane = quad(resources, 0xe308, measured);
      remixapi_LightInfoDomeEXT dome{};
      dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
      for (uint32_t c = 0; c < 3; ++c) dome.transform.matrix[c][c] = 1;
      remixapi_LightInfo sky{};
      sky.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      sky.hash = 0xe309;
      sky.pNext = &dome;
      sky.radiance = {1, 1, 1};
      remixapi_LightHandle skyLight;
      resources.createLight(sky, skyLight);
      auto draw = [&](float distance, bool front = false, bool textured = false, bool skyBehind = false) {
        camera(resources);
        if (skyBehind) resources.drawLight(skyLight);
        else resources.drawInstance(instance(background, distance));
        auto surface = instance(textured ? texturedPane : pane, -.2f);
        surface.transform.matrix[0][0] = surface.transform.matrix[1][1] = .6f;
        resources.drawInstance(surface);
        if (front) resources.drawInstance(instance(foreground, -.4f));
        return target.render();
      };
      constexpr size_t centre = (32 * 64 + 32) * 4;
      const auto expected = prismDisplay({.5f, std::sqrt(.5f), 1});
      const auto nearImage = draw(.3f), farImage = draw(.45f);
      require(target.forward->statistics().draws == 2,
              "A measured finite sheet is submitted through Forward+ refraction");
      for (uint32_t c = 0; c < 3; ++c) {
        std::printf("Sheet %ux channel %u: nearImage %u farImage %u expected %.2f\n", samples, c,
            unsigned(nearImage[centre + c]), unsigned(farImage[centre + c]), expected[c] * 255);
        require(std::abs(float(nearImage[centre + c]) - expected[c] * 255) < 4 &&
                    std::abs(int(nearImage[centre + c]) - int(farImage[centre + c])) <= 1,
                "Sheet tint depends on thickness, not background distance, and creates no diffuse glow");
      }
      const auto front = draw(.3f, true);
      const auto blue = prismDisplay({0, 0, 1});
      for (uint32_t c = 0; c < 3; ++c)
        require(std::abs(float(front[centre + c]) - blue[c] * 255) < 4,
                "Foreground opaque geometry occludes a finite sheet");
      for (uint32_t slot = 0; slot < remix::gpuFrameCount + 2; ++slot) {
        const auto reused = draw(.3f);
        for (uint32_t c = 0; c < 3; ++c)
          require(std::abs(int(reused[centre + c]) - int(nearImage[centre + c])) <= 1,
                  "Finite-sheet descriptors remain stable across frame-slot reuse");
      }
      const auto textured = draw(.3f, false, true);
      const auto texturedExpected = prismDisplay({.5f * std::pow(64.f / 255, .25f),
          std::sqrt(.5f) * std::pow(128.f / 255, .25f), 1});
      for (uint32_t c = 0; c < 3; ++c)
        require(std::abs(float(textured[centre + c]) - texturedExpected[c] * 255) < 4,
                "Sheet tint textures contribute optical depth at the same measurement distance");
      const auto skyImage = draw(.3f, false, false, true);
      for (uint32_t c = 0; c < 3; ++c)
        require(std::abs(float(skyImage[centre + c]) - expected[c] * 255) < 4,
                "Finite sheets transmit the visible sky even when opaque depth is absent");
      const uint8_t striped[] = {64, 64, 64, 255, 192, 192, 192, 255};
      texture.hash = 0xe30a;
      texture.width = 2;
      texture.data = striped;
      texture.dataSize = sizeof(striped);
      resources.createTexture(texture, image);
      remixapi_MaterialTransmittanceScrollEXT scroll{};
      scroll.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_TRANSMITTANCE_SCROLL_EXT;
      scroll.pNext = &optical;
      scroll.rate[0] = .5f;
      info.pNext = &scroll;
      info.hash = 0xe30b;
#if defined(_WIN32)
      info.albedoTexture = L"0xe30a";
#else
      info.albedoTexture = "0xe30a";
#endif
      info.wrapModeU = info.wrapModeV = 1;
      resources.createMaterial(info, measured);
      const auto animatedPane = quad(resources, 0xe30c, measured);
      auto animated = [&](double seconds) {
        target.animationSeconds = seconds;
        camera(resources);
        resources.drawInstance(instance(background, .3f));
        auto surface = instance(animatedPane, -.2f);
        surface.transform.matrix[0][0] = surface.transform.matrix[1][1] = .6f;
        resources.drawInstance(surface);
        return target.render();
      };
      const auto first = animated(0), shifted = animated(1), wrapped = animated(2);
      const size_t sample = (32 * 64 + 24) * 4 + 2;
      require(std::abs(int(first[sample]) - int(shifted[sample])) > 10 &&
                  std::abs(int(first[sample]) - int(wrapped[sample])) <= 1,
              "Live sheet tint UVs animate and wrap independently of geometry at every MSAA level");
    }
  }
