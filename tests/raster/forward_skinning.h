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
      const auto previousReuses = target.forward->statistics().rigidUploadReuses;
      const uint32_t vertexColors[] = {0xffffffff, 0xff808080};
      remixapi_MeshHandle coloredReferences[2][2];
      for (uint32_t color = 0; color < 2; ++color)
        for (uint32_t tint = 0; tint < 2; ++tint)
          coloredReferences[color][tint] = quad(referenceResources, 98700 + color * 2 + tint,
                                               color ? referenceRed : referenceWhite, vertexColors[tint]);
      // Source can submit already-posed vertices as a new rigid mesh each
      // frame. Release its API handle immediately; queued frames must keep
      // their data, and only fully retired uploads may be overwritten by
      // subsequent meshes.
      const auto transientBegan = std::chrono::steady_clock::now();
      for (uint32_t step = 0; step < 24; ++step) {
        auto render = [&](bool transient) {
          auto &output = transient ? target : referenceTarget;
          auto &resources = output.raster;
          camera(resources);
          if (shadows) {
            resources.drawLight(transient ? local : referenceLight);
            resources.drawInstance(
                instance(transient ? receiver : rigid[0], .4f));
          }
          for (uint32_t actor = 0; actor < step % 4; ++actor) {
            const uint32_t color = (actor + step) % 2;
            const uint32_t tint = (actor + step / 2) % 2;
            const auto mesh =
                transient ? quad(resources, 98600 + step * 4 + actor,
                                 color ? red : white, vertexColors[tint])
                          : coloredReferences[color][tint];
            auto draw = instance(mesh, .25f);
            draw.transform.matrix[0][0] = draw.transform.matrix[1][1] =
                step % 2 ? .125f : .25f;
            draw.transform.matrix[0][3] = (float(actor) - 1) * .5f;
            draw.transform.matrix[1][3] = step % 3 ? -.25f : .25f;
            resources.drawInstance(draw);
            if (transient)
              resources.destroyMesh(mesh);
          }
          return output.render();
        };
        require(render(true) == render(false),
                "Retired rigid uploads preserve current vertices, materials "
                "and shadows across frame slots");
        require(target.forward->statistics().spareGeometryBytes <=
                    8 * 1024 * 1024,
                "Retired upload storage remains bounded");
      }
      require(target.forward->statistics().rigidUploadReuses > previousReuses,
              "Transient meshes actually exercise retired upload reuse");
      std::printf(
          "Transient upload fixture: MSAA %u, shadows %u, %.3f ms, %llu reused "
          "uploads\n",
          samples, unsigned(shadows),
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - transientBegan)
              .count(),
          static_cast<unsigned long long>(
              target.forward->statistics().rigidUploadReuses - previousReuses));
    }
  }
  std::puts("Forward+ skinned frame-slot reuse matches rigid pose references "
            "at 1x/2x/4x MSAA");
}
