#pragma once

void viewModelDepthCameras(remix::NativeResources &resources, float aspect = 1) {
  remixapi_CameraInfoParameterizedEXT parameters{};
  parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
  parameters.aspect = aspect;
  parameters.fovYInDegrees = 90;
  parameters.nearPlane = .05f;
  parameters.farPlane = 100;
  parameters.right = {1, 0, 0};
  parameters.up = {0, 1, 0};
  parameters.forward = {0, 0, 1};
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &parameters;
  for (auto type : {REMIXAPI_CAMERA_TYPE_WORLD, REMIXAPI_CAMERA_TYPE_VIEW_MODEL}) {
    camera.type = type;
    resources.setupCamera(camera);
  }
}
void forwardDepthPrepassTests() {
  for (uint32_t samples : {1u, 2u, 4u}) {
    for (bool viewModel : {false, true}) {
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
      texture.width = 2;
      texture.height = texture.depth = texture.mipLevels = 1;
      texture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
      texture.data = texels;
      texture.dataSize = sizeof(texels);
      remixapi_TextureHandle handle;
      r.createTexture(texture, handle);
#if defined(_WIN32)
      const auto path = L"0x12910";
#else
      const auto path = "0x12910";
#endif
      const auto front = quad(r, 0x12911, material(r, 0x12912, 0, 1, 0, 1, path, 4));
      const auto back = quad(r, 0x12913, material(r, 0x12914, 0, 0, 1));
      const auto world = quad(r, 0x12915, material(r, 0x12916, 1, 0, 0));
      for (bool frontFirst : {false, true}) {
        auto render = [&] {
          if (viewModel)
            viewModelDepthCameras(r);
          else
            camera(r);
          if (viewModel)
            r.drawInstance(instance(world, -.3f));
          auto draw = [&](remixapi_MeshHandle mesh, float z) {
            auto info = instance(mesh, z);
            if (viewModel)
              info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
            r.drawInstance(info);
          };
          if (frontFirst)
            draw(front, -.1f);
          draw(back, .1f);
          if (!frontFirst)
            draw(front, -.1f);
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
                  "Depth prepass preserves cutout coverage, MSAA edges and "
                  "draw-order results");
        const auto left = 4 * (32 * 64 + 18), right = 4 * (32 * 64 + 46);
        std::printf("Depth cutout %u/%u viewmodel %u left %u %u %u right %u %u %u\n", samples,
                    unsigned(frontFirst), unsigned(viewModel), actual[left], actual[left + 1],
                    actual[left + 2], actual[right], actual[right + 1], actual[right + 2]);
        // The shared display transform can mix channels of saturated primaries.
        require(actual[left + 2] > 30 && actual[left + 2] > 4 * std::max(actual[left], actual[left + 1]) &&
                    actual[right + 1] > 30 &&
                    actual[right + 1] > 4 * std::max(actual[right], actual[right + 2]),
                "Cutout holes reveal the rear receiver while solid texels "
                "occlude it");
      }
    }
  }
}
