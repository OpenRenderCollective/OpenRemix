void forwardSkinningTests() {
  for (uint32_t samples : {1u, 2u, 4u}) {
    for (bool shadows : {false, true}) {
      Target target, referenceTarget;
      auto &r = target.raster;
      remix::forward::Options options;
      options.samples = samples;
      options.ambient = .2f;
      options.shadows = options.ssr = false;
      options.localShadows = shadows;
      options.localShadowLights = 1;
      options.localShadowResolution = 64;
      options.localShadowFade = 0;
      options.baking.enabled = false;
      options.baking.reflectionProbes = 0;
      target.enableForward(options);
      referenceTarget.enableForward(options);
      const auto white = material(r, 98501, 1, 1, 1);
      const auto red = material(r, 98502, 1, 0, 0);
      const auto receiver = quad(r, 98503, white);
      auto &referenceResources = referenceTarget.raster;
      const auto referenceWhite = material(referenceResources, 98501, 1, 1, 1);
      const auto referenceRed = material(referenceResources, 98502, 1, 0, 0);
      const remixapi_MeshHandle rigid[] = {quad(referenceResources, 98503, referenceWhite),
                                          quad(referenceResources, 98504, referenceRed)};
      const remixapi_MeshHandle skinned[] = {quad(r, 98505, white, 0xffffffff, true),
                                            quad(r, 98506, red, 0xffffffff, true)};
      remixapi_LightInfoSphereEXT sphere{};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      sphere.radius = .05f;
      remixapi_LightInfo light{};
      light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      light.pNext = &sphere;
      light.hash = 98507;
      light.radiance = {10, 10, 10};
      remixapi_LightHandle local{};
      r.createLight(light, local);
      remixapi_LightHandle referenceLight{};
      referenceResources.createLight(light, referenceLight);
      require(local && referenceLight, "Skinning fixture lights");
      // Alternate actor counts and order for longer than all fenced slots.
      // Rigid transformed quads provide an independent reference for each pose.
      for (uint32_t step = 0; step < 18; ++step) {
        const uint32_t count = step % 4;
        std::vector<remixapi_Transform> poses(count);
        for (uint32_t actor = 0; actor < count; ++actor) {
          auto &pose = poses[actor];
          pose.matrix[0][0] = pose.matrix[1][1] = pose.matrix[2][2] = step % 2 ? .125f : .25f;
          pose.matrix[0][3] = (float(actor) - 1) * .5f;
          pose.matrix[1][3] = step % 3 ? -.25f : .25f;
          pose.matrix[2][3] = .25f;
        }
        auto render = [&](bool skin) {
          auto &output = skin ? target : referenceTarget;
          auto &resources = output.raster;
          camera(resources);
          if (shadows) {
            resources.drawLight(skin ? local : referenceLight);
            resources.drawInstance(instance(skin ? receiver : rigid[0], .4f));
          }
          for (uint32_t actor = 0; actor < count; ++actor) {
            const uint32_t materialIndex = (actor + step) % 2;
            auto draw = instance(skin ? skinned[materialIndex] : rigid[materialIndex]);
            auto pose = poses[actor];
            remixapi_InstanceInfoBoneTransformsEXT bones{};
            bones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
            bones.boneTransforms_values = &pose;
            bones.boneTransforms_count = 1;
            if (skin) draw.pNext = &bones;
            else draw.transform = pose;
            resources.drawInstance(draw);
            pose.matrix[0][3] = 100; // Submission must own its pose.
          }
          return output.render();
        };
        const auto actual = render(true);
        const auto reference = render(false);
        require(actual == reference,
                "Reused pose buffers preserve moving geometry, independent actors and current shadows");
      }
      auto emptySlots = [&] {
        for (uint32_t frame = 0; frame < 2 * remix::gpuFrameCount; ++frame) {
          camera(r);
          if (shadows) {
            r.drawLight(local);
            r.drawInstance(instance(receiver, .4f));
          }
          target.render();
        }
        return target.forward->statistics().bufferBytes;
      };
      const auto emptyBytes = emptySlots();
      for (uint32_t frame = 0; frame < 2 * remix::gpuFrameCount; ++frame) {
        camera(r);
        auto pose = instance(skinned[frame % 2]).transform;
        pose.matrix[0][3] = float(frame % 2) - .5f;
        remixapi_InstanceInfoBoneTransformsEXT bones{};
        bones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
        bones.boneTransforms_values = &pose;
        bones.boneTransforms_count = 1;
        auto draw = instance(skinned[frame % 2]);
        draw.pNext = &bones;
        r.drawInstance(draw);
        target.render();
      }
      require(emptySlots() <= emptyBytes, "Unused pose buffers retire after their owning slots complete");
    }
  }
  std::puts("Forward+ skinned frame-slot reuse matches rigid pose references at 1x/2x/4x MSAA");
}
