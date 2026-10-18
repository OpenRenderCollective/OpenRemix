#pragma once

  void forwardDepthPrepassTests() {
    for (uint32_t samples : {1u, 2u, 4u}) {
      Target target;
      auto &r = target.raster;
      remix::forward::Options options;
      options.samples = samples;
      options.ambient = 1;
      options.shadows = options.localShadows = options.ssr = false;
      options.baking.enabled = false;
      options.depthPrepass = false;
      target.enableForward(options);
      const uint8_t texels[] = {255, 255, 255, 0, 255, 255, 255, 255};
      remixapi_TextureInfo texture{};
      texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
      texture.hash = 0x12910;
      texture.width = 2; texture.height = texture.depth = texture.mipLevels = 1;
      texture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
      texture.data = texels; texture.dataSize = sizeof(texels);
      remixapi_TextureHandle handle;
      r.createTexture(texture, handle);
#if defined(_WIN32)
      const auto path = L"0x12910";
#else
      const auto path = "0x12910";
#endif
      const auto front = quad(r, 0x12911, material(r, 0x12912, 0, 1, 0, 1, path, 4));
      const auto back = quad(r, 0x12913, material(r, 0x12914, 0, 0, 1));
      for (bool frontFirst : {false, true}) {
        auto render = [&] {
          camera(r);
          if (frontFirst) r.drawInstance(instance(front, -.1f));
          r.drawInstance(instance(back, .1f));
          if (!frontFirst) r.drawInstance(instance(front, -.1f));
          return target.render();
        };
        options.depthPrepass = false;
        target.forward->updateLighting(options);
        const auto reference = render();
        options.depthPrepass = true;
        target.forward->updateLighting(options);
        const auto actual = render();
        for (size_t i = 0; i < reference.size(); ++i)
          require(std::abs(int(actual[i]) - int(reference[i])) <= 1,
                  "Depth prepass preserves cutout coverage, MSAA edges and draw-order results");
        const auto left = 4 * (32 * 64 + 18), right = 4 * (32 * 64 + 46);
        std::printf("Depth cutout %u/%u left %u %u %u right %u %u %u\n", samples,
                    unsigned(frontFirst), actual[left], actual[left + 1], actual[left + 2],
                    actual[right], actual[right + 1], actual[right + 2]);
        // The shared display transform can mix channels of saturated primaries.
        require(actual[left + 2] > 30 && actual[left + 2] > 4 * std::max(actual[left], actual[left + 1]) &&
                    actual[right + 1] > 30 && actual[right + 1] > 4 * std::max(actual[right], actual[right + 2]),
                "Cutout holes reveal the rear receiver while solid texels occlude it");
      }
    }
  }
