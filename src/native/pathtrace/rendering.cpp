#include "pathtrace/gpu.h"
#include "pathtrace/sharc_policy.h"
#include "scene/emission.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  void PathTracer::Gpu::prepare(VkCommandBuffer command, RasterRenderer &resources,
                                const TraceOptions &requestedOptions, scene::TraceFrame &prepared) {
    // FrameBreakdownEXT phases: each lap charges the time since the previous one.
    auto lapStart = std::chrono::steady_clock::now();
    auto lap = [&](double &phase) {
      const auto now = std::chrono::steady_clock::now();
      phase += std::chrono::duration<double, std::milli>(now - lapStart).count();
      lapStart = now;
    };
    struct FinalLap {
      decltype(lap) &charge;
      double &phase;
      ~FinalLap() { charge(phase); }
    } finalLap{lap, breakdown.passesMilliseconds};
    const auto options = effectiveOptions(requestedOptions);
    // rtx.native.pathtrace.hardwareFaceCulling as this frame's partitions were
    // proved with it (prepareScene): BLAS opacity, instance flags and ray flags
    // must agree.
    const bool faceCulling = opacityCache.oneSidedOpaqueEnabled();
    debugView = options.debugView;
    const bool falseColor = debugView != 0 && debugView != 6 && debugView != 7;
    prism = options.prism && !falseColor;
    gamma22 = options.gamma22 && !falseColor;
    if (timestamps) {
      vkCmdResetQueryPool(command, timestamps, 0, 12);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 0);
    }
    complete(command);
    configureProfile(options);
    if (options.localDirectClamp != previousDirectClamp || options.indirectClamp != previousIndirectClamp ||
        options.localFireflyFilter != previousFireflyFilter) {
      history.clear();
      stats.accumulationCount = 0;
      previousDirectClamp = options.localDirectClamp;
      previousIndirectClamp = options.indirectClamp;
      previousFireflyFilter = options.localFireflyFilter;
    }
    rendered = false;
    pendingTime = std::chrono::steady_clock::now();
    rouletteActive =
        options.russianRoulette && options.rouletteMinBounces < options.bounces && !rouletteFailed;
    const uint64_t integrator =
        options.samples | uint64_t(options.bounces) << 8 | uint64_t(options.accumulate) << 16 |
        uint64_t(bool(denoiser) && postReblur) << 17 | uint64_t(bool(denoiser) && postTaa) << 18 |
        uint64_t(options.rasterPrimary) << 19 | uint64_t(bool(sharc.stats.active)) << 20 |
        uint64_t(options.sharcDebug) << 21 | uint64_t(options.pbrLocalReuse) << 22 |
        uint64_t(options.russianRoulette) << 23 | uint64_t(options.rouletteMinBounces) << 24 |
        uint64_t(rouletteFailed) << 27;
    history.begin(resources.temporalEpoch(), integrator, extent.width, extent.height);
    pendingVertices.clear();
    pendingVertices.reserve(previousVertices.size());
    const auto emissionMode = options.freezeEmissiveRadiance      ? 2u
                              : options.stabilizeEmissiveLighting ? 1u
                                                                  : 0u;
    std::vector<VkDescriptorImageInfo> textures;
    // The scene signature: sky draws first, the tables' segments, then the
    // frame's lighting and settings (append writes to the current segment).
    trace::SceneSignature signature;
    std::vector<uint8_t> *sceneBytes = &signature.head;
    auto append = [&](const void *data, size_t size) {
      const auto *bytes = static_cast<const uint8_t *>(data);
      sceneBytes->insert(sceneBytes->end(), bytes, bytes + size);
    };
    for (const auto &draw : resources.draws())
      if (draw.camera == REMIXAPI_CAMERA_TYPE_SKY) {
        append(draw.model.data(), sizeof(draw.model));
        const auto material = scene::TemporalHistory::materialSignature(draw);
        append(&material, sizeof(material));
        append(draw.surface->vertices.data(), draw.surface->vertices.size() * sizeof(scene::Vertex));
        append(draw.surface->indices.data(), draw.surface->indices.size() * sizeof(uint32_t));
        const auto &camera = resources.camera(REMIXAPI_CAMERA_TYPE_SKY);
        append(camera.data(), sizeof(camera));
        if (draw.material->fps) {
          auto animationFrame = uint64_t(prepared.seconds * draw.material->fps);
          append(&animationFrame, sizeof(animationFrame));
        }
      }
    sceneBytes = &signature.frame;
    // Dynamic textures (remixapi_UpdateTextureEXT) keep their fingerprint:
    // what their images show is part of the scene through this count.
    {
      const uint64_t dynamicTextures = resources.dynamicTextureGeneration();
      append(&dynamicTextures, sizeof(dynamicTextures));
    }
    // Units of a static set that is not traced any more: once no committed
    // frame needs their history, let them go.
    if (!prepared.staticUnits && !resources.persistentStatic() && !statics.units().empty() &&
        statics.committedAbsent()) {
      tables.reset();
      statics.clear();
    }
    lap(breakdown.traceSetupMilliseconds);
    if (!workers)
      workers = std::make_unique<WorkerPool>();
    // The frame's tables: surface table, primary draws, TLAS inputs, SHARC
    // and signature bytes, texture descriptors and temporal staging. With
    // the persistent static set, its units' parts are kept across frames and
    // only changed units are built again (frame_tables.h).
    history.setFallback(&statics);
    // Geometry and texture uploads are recorded while the tables are built.
    gpu::GpuScope uploadTime(command, "Scene uploads (geometry, textures)");
    assembleJobs.clear(); // Only this command buffer's copies feed its jobs.
    TableViews views{this, command};
    const trace::FrameTables<TableViews>::Inputs tableInputs{prepared,
                                                             &statics,
                                                             history,
                                                             previousVertices,
                                                             pendingVertices,
                                                             *workers,
                                                             ++prepareSerial,
                                                             options.pbrLocalReuse,
                                                             digestEnabled,
                                                             PathTracer::textureLimit,
                                                             &materialTicks,
                                                             &temporalTicks};
    tables.build(views, tableInputs);
    {
      const auto &s = tables.statistics();
      auto &out = staticStatistics;
      out.active = prepared.staticUnits;
      out.units = s.staticUnits;
      out.surfaces = tables.prefixSurfaces();
      out.reusedSurfaces = s.staticReused;
      out.rebuiltSurfaces = s.staticRebuilt;
      out.dynamicSurfaces = s.dynamicSurfaces;
      out.rebuiltUnits = s.rebuiltUnits;
      out.temporalSurfaces = s.temporalSurfaces;
      out.relayouts = s.relayouts;
      out.tableMilliseconds = s.staticMilliseconds;
      if (staticValidation && prepared.staticUnits) {
        ++out.validations;
        const auto mismatch = tables.validate(views, tableInputs);
        if (!mismatch.empty()) {
          ++out.mismatches;
          std::snprintf(out.mismatch, sizeof(out.mismatch), "%s", mismatch.c_str());
          if (!staticMismatchLogged) {
            staticMismatchLogged = true;
            std::fprintf(stderr, "Persistent static set: kept tables differ from a rebuild: %s\n",
                         mismatch.c_str());
          }
        }
      }
    }
    const auto flags = tables.flags();
    uint64_t reuseSignature = tables.reuseSignature();
    const bool hasCacheReceivers =
        flags.receivers || (options.sharcGlossySurfaces && flags.sharpReceivers) ||
        (options.sharcDynamicSurfaces &&
         (flags.dynamicReceivers || (options.sharcGlossySurfaces && flags.dynamicSharpReceivers)));
    const bool hasBlendedGeometry = flags.blended;
    decalMaterialsActive = flags.decal;
    shadowEarlyOutActive = options.shadowEarlyOut;
    // Texture descriptors in first-use order: the static prefix's (resolved
    // again only when the prefix changes) and the frame's other materials.
    {
      ScopedTicks time{materialTicks};
      auto sampled = [&](const trace::DescriptorRequest &request) {
        switch (request.kind) {
        case trace::DescriptorRequest::Albedo:
          return resources.sampledMaterial(request.material, command);
        case trace::DescriptorRequest::Map:
          return resources.sampledTexture(request.material, *request.texture, command);
        default:
          return resources.sampledEmission(request.material, command);
        }
      };
      textures.reserve(tables.requestCount() + 1);
      if (const auto *prefix = tables.prefix()) {
        if (prefixTexturesGeneration != prefix->generation) {
          prefixTextures.clear();
          for (const auto &request : prefix->requests)
            prefixTextures.push_back(sampled(request));
          prefixTexturesGeneration = prefix->generation;
        }
        textures = prefixTextures;
      }
      for (const auto &request : tables.tail().requests)
        textures.push_back(sampled(request));
    }
    uploadTime.close();
    breakdown.traceInstances += tables.instanceCount();
    lap(breakdown.traceSurfacesMilliseconds);
    breakdown.traceSurfaces += tables.surfaceCount();
    breakdown.primaryDraws += tables.primaryCount();
    // Retire absent deformation instances after their last-use fence. Rigid
    // geometry remains cached for the lifetime of its shared CPU mesh.
    // A view's owner can only have expired if a CPU mesh was released since
    // the last full scan; unused skins retire every frame.
    auto retire = [&](decltype(meshes)::iterator it) {
      retiredGeometry.push_back(std::move(it->second.geometry));
      for (auto &vertices : it->second.vertices)
        retiredBuffers.push_back(std::move(vertices));
      deformViews.erase(&it->first);
      return meshes.erase(it);
    };
    const uint64_t releases = scene::meshReleases.load(std::memory_order_acquire);
    if (releases != meshReleasesSeen) {
      meshReleasesSeen = releases;
      for (auto it = meshes.begin(); it != meshes.end();) {
        if (it->second.owner.expired() || (std::get<1>(it->first) && it->second.lastUse != sequence))
          it = retire(it);
        else
          ++it;
      }
    } else if (!deformViews.empty()) {
      std::vector<const MeshViewKey *> unused;
      for (const auto *key : deformViews) {
        const auto found = meshes.find(*key);
        if (found != meshes.end() && (found->second.owner.expired() || found->second.lastUse != sequence))
          unused.push_back(key);
      }
      for (const auto *key : unused)
        retire(meshes.find(*key));
    }
    for (auto it = updateViews.begin(); it != updateViews.end();) {
      auto view = meshes.find(it->second);
      if (view == meshes.end() || view->second.updateStream.get() != std::get<0>(it->first))
        it = updateViews.erase(it);
      else
        ++it;
    }
    lap(breakdown.retireMilliseconds);
    {
      gpu::GpuScope scope(command, "Delta vertex assembly");
      recordAssemble(command);
    }
    // Geometry uploads (and delta-assembled shading vertices) feed both AS
    // construction and BDA reads in the compute shader.
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            buildStage | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            asRead | VK_ACCESS_SHADER_READ_BIT);
    {
      gpu::GpuScope scope(command, "BLAS builds, refits, compaction");
      buildBottom(command);
      // Compaction is also an AS write, including on a frame with no new BLAS work.
      barrier(command, buildStage, asWrite, buildStage, asRead);
    }
    {
      gpu::GpuScope scope(command, "TLAS builds");
      buildTop(command, tables.staticInputs(), tables.dynamicInputs(), options.forceRebuild,
               tables.staticGeneration(), faceCulling, options.combinedTlas);
      barrier(command, buildStage, asWrite, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, asRead);
    }
    if (timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, timestamps, 1);
    lap(breakdown.traceAccelerationMilliseconds);
    trace::g_accelNow.total = trace::g_accelNow.bottom + trace::g_accelNow.top;
    if (trace::g_accelNow.total > trace::g_accelCosts.total)
      trace::g_accelCosts = trace::g_accelNow;
    trace::g_accelNow = {};
    // An empty frame uploads one default surface.
    const TraceSurface emptySurface{};
    const size_t prefixSurfaceCount = tables.prefixSurfaces(), tailSurfaceCount = tables.tail().surfaces.size();
    const bool noSurfaces = !prefixSurfaceCount && !tailSurfaceCount;
    const size_t surfaceCount = noSurfaces ? 1 : prefixSurfaceCount + tailSurfaceCount;
    const void *prefixData = prefixSurfaceCount ? tables.prefix()->surfaces.data() : nullptr;
    const void *tailData = noSurfaces ? &emptySurface : tables.tail().surfaces.data();
    const size_t prefixBytes = prefixSurfaceCount * sizeof(TraceSurface);
    const size_t tableBytes = surfaceCount * sizeof(TraceSurface);
    tables.signature(signature);
    if (digestEnabled) {
      // Device addresses depend on allocation order; hash everything else.
      digest = tables.geometryDigest();
      auto hashSurface = [&](const TraceSurface &surface) {
        auto copy = surface;
        const bool history = copy.previousVertices != 0;
        copy.vertices = copy.indices = copy.triangleMap = copy.traceIndices = 0;
        copy.previousVertices = history;
        digest = hashBytes(&copy, sizeof(copy), digest);
      };
      if (noSurfaces)
        hashSurface(emptySurface);
      for (size_t i = 0; i < prefixSurfaceCount + tailSurfaceCount; ++i)
        hashSurface(tables.surface(i));
      for (unsigned i = 0; i < 2; ++i)
        for (const auto &input : i ? tables.dynamicInputs() : tables.staticInputs()) {
          // boundsRevision counts rebuilds since startup: frame-count dependent.
          const uint64_t values[] = {i, input.id, input.sub, input.customIndex};
          digest = hashBytes(values, sizeof(values), digest);
          digest = hashBytes(input.model.data(), sizeof(input.model), digest);
          digest = hashBytes(&input.diagonal, sizeof(input.diagonal), digest);
        }
      for (size_t i = 0; i < tables.primaryCount(); ++i) {
        const auto &draw = tables.primary(i);
        const uint32_t values[] = {draw.surfaceIndex, draw.count, draw.blended};
        digest = hashBytes(draw.model.data(), sizeof(draw.model), digest);
        digest = hashBytes(values, sizeof(values), digest);
      }
    }
    lap(breakdown.signatureMilliseconds);
    gpu::GpuScope tableTime(command, "Surface table and light uploads");
    // A diagnostic placement switch replaces only this completed slot's table.
    if (table && table->host == deviceLocalScene) {
      table.reset();
      tableVersion = 0;
      tableShadow.clear();
    }
    using TableClock = std::chrono::steady_clock;
    const auto tableBegan = TableClock::now();
    const auto tableSince = [](TableClock::time_point t) {
      return std::chrono::duration<double, std::milli>(TableClock::now() - t).count();
    };
    trace::TableCosts tableCosts;
    tableCosts.bytes = tableBytes;
    // A table that no longer fits grows by half again, not to the size asked:
    // a scene gaining instances one at a time (props, ragdolls, NPCs spawned
    // in play) would otherwise replace both buffers of every slot at each
    // one. The old buffers go to the idle cache, not back to the driver.
    const size_t tableCapacity = (tableBytes + tableBytes / 2 + 65535) & ~size_t(65535);
    if (deviceLocalScene) {
      // Staged whole (a host write), copied to the device in the blocks that
      // differ from what this slot's table holds: a static world costs no
      // copy, and the kept static prefix is not even compared.
      if (!tableUpload || tableUpload->size < tableBytes) {
        tableUpload = buffer(tableCapacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, true);
        ++tableCosts.grown;
      }
      if (!table || table->size < tableBytes) {
        table = buffer(tableCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false,
                       true);
        tableShadow.clear(); // a new buffer holds nothing yet
        ++tableCosts.grown;
      }
      tableCosts.grow = tableSince(tableBegan);
      tableCosts.capacity = table->size;
      const size_t held = tableShadow.size();
      const bool kept = held >= prefixBytes && tableVersion && tableVersion == tables.prefixVersion();
      const size_t from = kept ? prefixBytes : 0;
      const auto writeBegan = TableClock::now();
      write(*tableUpload, {{prefixData, prefixBytes}, {tailData, tableBytes - prefixBytes}}, from);
      tableCosts.write = tableSince(writeBegan);
      const auto compareBegan = TableClock::now();
      if (tableShadow.size() < tableBytes)
        tableShadow.resize(tableBytes);
      std::vector<VkBufferCopy> regions;
      auto changed = [&](const void *data, size_t bytes, size_t base) {
        constexpr size_t block = 4096;
        const auto *source = static_cast<const uint8_t *>(data);
        for (size_t at = 0; at < bytes; at += block) {
          const size_t size = std::min(block, bytes - at), offset = base + at;
          if (offset + size <= held && !std::memcmp(tableShadow.data() + offset, source + at, size))
            continue;
          std::memcpy(tableShadow.data() + offset, source + at, size);
          if (!regions.empty() && regions.back().srcOffset + regions.back().size == offset)
            regions.back().size += size;
          else
            regions.push_back({.srcOffset = offset, .dstOffset = offset, .size = size});
        }
      };
      if (!kept)
        changed(prefixData, prefixBytes, 0);
      changed(tailData, tableBytes - prefixBytes, prefixBytes);
      if (regions.size() > 64) // scattered changes: one copy over their span
        regions = {{.srcOffset = regions.front().srcOffset,
                    .dstOffset = regions.front().srcOffset,
                    .size = regions.back().srcOffset + regions.back().size - regions.front().srcOffset}};
      if (!regions.empty())
        vkCmdCopyBuffer(command, tableUpload->handle, table->handle, uint32_t(regions.size()), regions.data());
      for (const auto &region : regions)
        tableCosts.copied += region.size;
      tableCosts.compare = tableSince(compareBegan);
      tableVersion = tables.prefixVersion();
    } else {
      // This slot's table may already hold the kept static prefix.
      bool kept = table && table->size >= tableBytes && tableVersion && tableVersion == tables.prefixVersion();
      if (!table || table->size < tableBytes) {
        // (Room to spare and part of a block, as hostBuffer's.)
        if (table)
          retiredBuffers.push_back(std::move(table));
        table = buffer(tableBytes + tableBytes / 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true);
        kept = false;
      }
      write(*table, {{prefixData, prefixBytes}, {tailData, tableBytes - prefixBytes}}, kept ? prefixBytes : 0);
      tableVersion = tables.prefixVersion();
    }
    tableCosts.total = tableSince(tableBegan);
    if (tableCosts.total > trace::g_tableCosts.total)
      trace::g_tableCosts = tableCosts;
    lap(breakdown.tableMilliseconds);
    // Emission constants, UVs and texture fingerprints already participate in
    // the scene signature above. Do not duplicate every triangle's reduction.
    auto &lighting = prepared.lighting;
    // Emissive sets select their own estimator (candidate count, solid-angle
    // sampling, split signals), and switching it resets the guiding history.
    // When the last emissive surface goes (the only glowing prop leaves the
    // host's view) the selection stays for a while, so a prop that returns
    // does not switch twice; the lights sampled are the frame's either way.
    if (lighting.hasEmissive)
      emissiveHold = 300;
    else if (!options.emissiveLights || !options.localLights)
      emissiveHold = 0;
    else if (emissiveHold)
      --emissiveHold;
    const bool hasEmissiveLights = lighting.hasEmissive || emissiveHold;
    emissiveSeen |= hasEmissiveLights;
    emissionHistory = std::move(lighting.history);
    lightBvh = std::move(lighting.combined);
    lightVisibility = std::move(lighting.visibility);
    // The family trees come back whether or not this frame uses them: they
    // are the state the next update starts from (an unused tree is uploaded
    // when it is next used: its generation differs).
    if (lighting.families) {
      torchBvh = std::move(lighting.torch);
      emissiveBvh = std::move(lighting.emissive);
    }
    lighting.staged = false;
    emittingInstances = lighting.emittingInstances;
    dynamicEmittingInstances = lighting.dynamicEmittingInstances;
    lightUpdateMilliseconds = lighting.updateMilliseconds;
    if (++lightUpdatePeakFrames >= 300) {
      lightUpdatePeakFrames = 0;
      lightUpdatePeak[1] = std::exchange(lightUpdatePeak[0], 0.);
    }
    lightUpdatePeak[0] = std::max(lightUpdatePeak[0], lighting.updateMilliseconds);
    // Potentially visible light table and the partition it indexes: replaced
    // only when they change (rare), like the light tree's buffers.
    // They are shared by the frame slots: a replaced buffer stays until the
    // frames in flight that read it have completed.
    auto retireBuffer = [&](std::unique_ptr<Buffer> &out) {
      if (out)
        retiredBuffers.push_back(std::move(out));
    };
    if (!lightVisibility.table()) {
      retireBuffer(visibilityTable);
      retireBuffer(visibilityNodes);
      retireBuffer(visibilityLeaves);
      visibilityTableVersion = visibilityPartitionVersion = 0;
    } else {
      const auto &partition = *lightVisibility.partition();
      auto place = [&](std::unique_ptr<Buffer> &out, const void *data, size_t size) {
        if (deviceLocalScene) {
          retireBuffer(out);
          out = upload(command, data, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        } else {
          // As for the light tree (uploadLights): replace GPU-only allocations.
          if (out && !out->host)
            retireBuffer(out);
          hostBuffer(out, data, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
      };
      if (visibilityPartitionVersion != partition.version || (visibilityNodes && visibilityNodes->host == deviceLocalScene)) {
        visibilityPartitionVersion = partition.version;
        place(visibilityNodes, partition.nodes.data(), partition.nodes.size() * sizeof(partition.nodes[0]));
        place(visibilityLeaves, partition.leafClusters.data(),
              std::max<size_t>(1, partition.leafClusters.size()) * sizeof(int32_t));
      }
      if (visibilityTableVersion != lightVisibility.version() || (visibilityTable && visibilityTable->host == deviceLocalScene)) {
        visibilityTableVersion = lightVisibility.version();
        const auto &table = *lightVisibility.table();
        place(visibilityTable, table.data(), std::max<size_t>(1, table.size()) * sizeof(uint32_t));
        ++visibilityTableUploads;
      }
    }
    localAngularActive = hasEmissiveLights && options.emissiveSolidAngleSampling;
    // A denoiser built for split local lighting filters only the split pass's
    // signal: run that pass whenever it is, even with API (torch) lights only,
    // or their radiance reaches the image unfiltered through the opaque pass.
    splitLightingActive = options.separateLocalLighting != 0 &&
                          (postSplit || ((hasEmissiveLights || options.restir) && (!denoiser || rrActive())));
    auto uploadLights = [&](scene::LightBvh &bvh, uint64_t &uploadedGeneration, uint64_t &uploadedInstance,
                            std::unique_ptr<Buffer> &nodes, std::unique_ptr<Buffer> &emitters) {
      if ((!nodes || nodes->host == deviceLocalScene || uploadedGeneration != bvh.generation ||
           uploadedInstance != bvh.instance()) &&
          !bvh.nodes().empty()) {
        const size_t nodeBytes = bvh.nodes().size() * sizeof(scene::LightNode);
        const size_t emitterBytes = bvh.emitters().size() * sizeof(scene::LightEmitter);
        if (deviceLocalScene) {
          // This slot's own buffers, whose fence has completed: rewritten in
          // place while they are large enough. A new buffer per change (lights
          // that flicker or come and go change the set every frame, and its
          // size with it) allocated device memory on those frames, tens of ms
          // at times. Where the slot holds an earlier generation of the same
          // hierarchy, only what changed since is copied.
          const bool partial = nodes && emitters && !nodes->host && !emitters->host && nodes->size >= nodeBytes &&
                               emitters->size >= emitterBytes &&
                               bvh.changedSince(uploadedInstance, uploadedGeneration, changedEmitters, changedNodes);
          const auto place = [&](std::unique_ptr<Buffer> &out, const void *data, size_t size) {
            if (!out || out->host || out->size < size)
              out = buffer(size + size / 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           false, true);
            const VkBufferCopy copy = {.srcOffset = stage(data, size), .size = size};
            vkCmdCopyBuffer(command, staging.buffer->handle, out->handle, 1, &copy);
            trace::g_lightUploads.bytes += size;
            ++trace::g_lightUploads.regions;
          };
          const auto patch = [&](Buffer &out, const auto *data, const scene::LightBvh::Ranges &ranges) {
            if (ranges.empty())
              return;
            // Packed into one staging write (each stage() flushes).
            constexpr size_t stride = sizeof(*data);
            std::vector<VkBufferCopy> copies;
            copies.reserve(ranges.size());
            auto &packed = lightUploadPacking;
            packed.clear();
            for (const auto &[first, end] : ranges) {
              const size_t size = (end - first) * stride;
              copies.push_back({.srcOffset = packed.size(), .dstOffset = first * stride, .size = size});
              const auto *bytes = reinterpret_cast<const uint8_t *>(data + first);
              packed.insert(packed.end(), bytes, bytes + size);
            }
            const VkDeviceSize base = stage(packed.data(), packed.size());
            for (auto &copy : copies)
              copy.srcOffset += base;
            trace::g_lightUploads.bytes += packed.size();
            vkCmdCopyBuffer(command, staging.buffer->handle, out.handle, uint32_t(copies.size()), copies.data());
            trace::g_lightUploads.regions += copies.size();
          };
          ++(partial ? trace::g_lightUploads.partial : trace::g_lightUploads.whole);
          if (partial) {
            patch(*nodes, bvh.nodes().data(), changedNodes);
            patch(*emitters, bvh.emitters().data(), changedEmitters);
          } else {
            place(nodes, bvh.nodes().data(), nodeBytes);
            place(emitters, bvh.emitters().data(), emitterBytes);
          }
        } else {
          // This slot's fence has completed; replace GPU-only allocations before
          // hostBuffer tries to map them when a diagnostic switches back.
          if (nodes && !nodes->host)
            nodes.reset();
          if (emitters && !emitters->host)
            emitters.reset();
          hostBuffer(nodes, bvh.nodes().data(), nodeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
          hostBuffer(emitters, bvh.emitters().data(), emitterBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        }
        uploadedGeneration = bvh.generation;
        uploadedInstance = bvh.instance();
      }
    };
    const bool splitFamilies = splitLightingActive && options.separateLocalLighting != 3;
    if (splitFamilies) {
      uploadLights(torchBvh, torchBufferGeneration, torchBufferInstance, torchNodes, torchEmitters);
      uploadLights(emissiveBvh, emissiveBufferGeneration, emissiveBufferInstance, emissiveNodes, emissiveEmitters);
    }
    uploadLights(lightBvh, lightBufferGeneration, lightBufferInstance, lightNodes, lightEmitters);
    if (deviceLocalScene)
      barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT);
    tableTime.close();
    Frame frame = {};
    frame.localConfig[0] = options.localLights && options.localIntensity > 0 ? lightBvh.active : 0;
    localIlluminationActive = frame.localConfig[0] != 0;
    localFirstHitActive = localIlluminationActive && !hasBlendedGeometry;
    frame.splitLocal[0] = splitLightingActive ? options.separateLocalLighting : 0;
    frame.splitLocal[1] = localIlluminationActive && splitFamilies ? torchBvh.active : 0;
    frame.splitLocal[2] = localIlluminationActive && splitFamilies ? emissiveBvh.active : 0;
    frame.splitLocal[3] = debugView;
    frame.localConfig[1] = options.localSamples;
    frame.localConfig[2] = uint32_t(lightBvh.lightingGeneration);
    frame.localConfig[3] =
        uint32_t(lightBvh.lightingGeneration >> 32) | (options.adaptiveLocalSampling ? 0x80000000u : 0u) |
        (hasEmissiveLights ? 0u : 0x40000000u); // sphere-only set: guide sphere lights too
    frame.localScale[0] = options.localIntensity;
    frame.localScale[1] = options.localRange;
    // Private, previously unused shader word: bounded candidate count. Keep
    // the established one-/many-sphere and <=4-emitter estimators unchanged.
    frame.localScale[2] = lightBvh.emitters().size() <= 4 ? 1.f
                          : hasEmissiveLights           ? float(options.emissiveCandidates)
                                                        : float(options.sphereCandidates);
    localResamplingActive = localIlluminationActive && frame.localScale[2] > 1;
    frame.localScale[3] = options.localRange > 0 ? 1.f / (options.localRange * options.localRange) : 0;
    frame.camera = resources.camera();
    primaryReverseDepth = resources.reverseDepth();
    // Camera rays: invert the view and the projection separately. The camera
    // matrix's clip z and w rows both carry the eye's translation and differ by
    // the near plane, which single precision loses far from the origin (most
    // with a near plane moved in for view models): ray origins would wander
    // and near geometry (the gun) jitter as the camera moves.
    frame.inverseCamera = scene::inverse(frame.camera);
    const auto eye = scene::inverse(resources.view());
    std::copy_n(eye.data() + 12, 3, frame.eye);
    // Camera rays: the view and the projection inverted separately, without
    // the eye's translation (Frame::inverseCameraRelative).
    try {
      auto rotation = eye;
      rotation[12] = rotation[13] = rotation[14] = 0;
      frame.inverseCameraRelative = scene::multiply(rotation, scene::inverse(resources.projection()));
    } catch (...) {
      scene::Matrix toEye = scene::identity();
      for (int c = 0; c < 3; ++c)
        toEye[12 + c] = -frame.eye[c];
      frame.inverseCameraRelative = scene::multiply(toEye, frame.inverseCamera);
    }
    auto light = resources.light();
    if (options.overrideLight) {
      float norm =
          std::hypot(options.lightDirection[0], options.lightDirection[1], options.lightDirection[2]);
      if (norm > 1e-6f)
        for (int i = 0; i < 3; ++i)
          light.direction[i] = options.lightDirection[i] / norm;
      for (int i = 0; i < 3; ++i)
        light.radiance[i] = options.lightColor[i] * options.lightIntensity;
    }
    const float diameter =
        options.sunAngularDiameter >= 0 ? options.sunAngularDiameter : light.angularDiameterDegrees;
    frame.sun[0] = std::tan(diameter * (3.14159265359f / 360.f));
    frame.sun[1] = denoiser && postSigma;
    configureBlueNoise(command, frame, options);
    frame.sun[2] = frame.sun[1] != 0 ? float(options.shadowDebug) : 0;
    const char *localDebug = std::getenv("REMIX_EMISSIVE_HIT_DISTANCE");
    frame.sun[3] = localDebug ? (std::strcmp(localDebug, "budget") == 0 ? 2.f : 1.f) : 0.f;
    std::copy_n(light.direction, 3, frame.direction);
    std::copy_n(light.radiance, 3, frame.radiance);
    if (options.sky)
      std::copy_n(resources.skyRadiance().data(), 3, frame.skyRadiance);
    if (options.sky && resources.dome().enabled) {
      const auto &dome = resources.dome();
      std::copy_n(dome.radiance.data(), 3, frame.skyRadiance);
      frame.skyRadiance[3] = -1; // Uniform authored dome also draws the background.
      for (unsigned row = 0; row < 3; ++row)
        for (unsigned col = 0; col < 3; ++col)
          frame.domeTransform[row][col] = dome.worldToLocal[col * 4 + row];
      if (dome.texture) {
        if (textures.size() >= PathTracer::textureLimit)
          throw VK_ERROR_TOO_MANY_OBJECTS;
        // Nearest sampling showed every dome texel as a block on screen.
        auto material = resources.domeMaterial();
        frame.skyRadiance[3] = float(textures.size() + 1);
        textures.push_back(resources.sampledTexture(material, dome.texture, command));
        auto fingerprint = dome.texture->fingerprint();
        append(&fingerprint, sizeof(fingerprint));
      }
    }
    // Light cookies: each slot's texture in this frame's texture table (+ 1).
    {
      const auto &cookies = resources.cookieTextures();
      std::vector<uint32_t> table(cookies.size());
      for (size_t i = 0; i < cookies.size(); ++i) {
        if (!cookies[i])
          continue;
        if (textures.size() >= PathTracer::textureLimit)
          throw VK_ERROR_TOO_MANY_OBJECTS;
        table[i] = uint32_t(textures.size() + 1);
        textures.push_back(resources.sampledTexture(resources.cookieMaterial(), cookies[i], command));
        auto fingerprint = cookies[i]->fingerprint();
        append(&fingerprint, sizeof(fingerprint));
      }
      if (lightCookies)
        retiredBuffers.push_back(std::move(lightCookies));
      if (!table.empty())
        hostBuffer(lightCookies, table.data(), table.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      frame.lightCookies = lightCookies ? lightCookies->address : 0;
    }
    // Role CUT (remixapi_InstanceInfoMaskEXT): the coverage of the groups
    // whose instances the camera does not see, drawn by the raster renderer
    // ahead of this frame's passes. Its sources are in no table: their
    // digest stands for them in the scene signature.
    if (const auto cut = resources.cutCoverage();
        cut.count && textures.size() + cut.count <= PathTracer::textureLimit) {
      frame.cutConfig[0] = uint32_t(textures.size() + 1);
      frame.cutConfig[1] = cut.count;
      std::copy_n(cut.slots, 4, frame.cutSlots);
      textures.insert(textures.end(), cut.images, cut.images + cut.count);
      append(&cut.digest, sizeof(cut.digest));
    }
    // remixapi_InstanceInfoCameraCullingEXT: what the camera sees of the scene
    // is part of its signature (the flags applied, the visible set).
    configureCameraCulling(command, frame, options, resources);
    append(frame.cameraCull, sizeof(frame.cameraCull));
    if (frame.cameraLeaves)
      append(cameraCulling.leavesClusters, sizeof(cameraCulling.leavesClusters));
    frame.config[0] = options.samples;
    std::copy_n(resources.cameraMedium().data(), 4, frame.cameraMedium);
    frame.upAxis = float(std::min(options.volumetricUpAxis, 2u));
    {
      // Scrolling maps (water): wrapped well before float precision suffers.
      static const auto start = std::chrono::steady_clock::now();
      const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      frame.animationTime = float(std::fmod(seconds, 4096.0));
    }
    frame.config[1] = options.bounces;
    frame.target[0] = extent.width;
    frame.target[1] = extent.height;
    // Bits 0/1: TLAS bindings 0/1 non-empty (the combined TLAS is bound to
    // both and queried once, through binding 0); bit 2: cull back faces (only
    // one-sided instances enable it, see trace::instanceFlags).
    const bool staticInstances = !tables.staticInputs().empty(), dynamicInstances = !tables.dynamicInputs().empty();
    frame.target[2] = (topCombined ? uint32_t(staticInstances || dynamicInstances)
                                   : uint32_t(staticInstances) | uint32_t(dynamicInstances) << 1) |
                      uint32_t(faceCulling) << 2;
    frame.target[3] = options.rasterPrimary;
    // Reserved private word; included in scene-change detection. Raw/reference
    // and multi-sample rendering retain one estimator at every surface pixel.
    frame.jitter[2] = options.waterIndirectReuse && options.indirectClamp == 0 && denoiser &&
                      (postTaa || upscaler) && !options.accumulate && options.samples == 1 &&
                      options.bounces > 0;
    frame.firefly[0] = options.localDirectClamp;
    frame.firefly[1] = options.indirectClamp;
    frame.pathConfig[0] = rouletteActive;
    frame.pathConfig[1] = options.rouletteMinBounces;
    frame.pathConfig[2] = (options.translucentIndirect ? 1u : 0u) | (options.sheetLocalLights ? 2u : 0u) |
                          (options.layerReuse ? 4u : 0u) | (options.shortSheetPaths ? 8u : 0u) |
                          std::clamp(options.skySamples, 1u, 16u) << 8 |
                          std::clamp(std::min(options.sheetSkySamples, options.skySamples), 1u, 16u) << 16;
    // Bits 0-7: the instance mask of shadow, light-visibility and bounce rays
    // (view models never; blended layers unless skipBlendedSecondaryRays is
    // off). Bit 8: a traced view model is in the frame (camera rays look for
    // it first; see cameraHit).
    frame.pathConfig[3] = (options.skipBlendedSecondaryRays ? 0xffu & ~uint32_t(trace::blendedInstanceMask) : 0xffu) &
                          ~uint32_t(trace::viewModelInstanceMask) & ~uint32_t(trace::reflectionOnlyInstanceMask);
    if (prepared.viewModel)
      frame.pathConfig[3] |= 0x100u;
    append(&frame, sizeof(frame));
    append(&options.localFireflyFilter, sizeof(options.localFireflyFilter));
    append(&options.translucencyDenoiser, sizeof(options.translucencyDenoiser));
    append(&options.translucencyReuse, sizeof(options.translucencyReuse));
    append(&lightBvh.generation, sizeof(lightBvh.generation));
    append(&integrator, sizeof(integrator));
    append(&options.validation, sizeof(options.validation));
    append(&options.localGuiding, sizeof(options.localGuiding));
    append(&options.shadowEarlyOut, sizeof(options.shadowEarlyOut));
    append(&options.emissiveSolidAngleSampling, sizeof(options.emissiveSolidAngleSampling));
    append(&emissionMode, sizeof(emissionMode));
    append(&options.fsrSharpness, sizeof(options.fsrSharpness));
    const uint64_t cacheSettings[] = {
        options.sharcReset,     options.sharcCapacityLog2,       options.sharcSceneScale,
        options.sharcDownscale, options.sharcAccumulationFrames, options.sharcStaleFrames,
        options.sharcMotionFrames, options.sharcMaxUpdatePaths, options.sharcDynamicSurfaces,
        options.sharcGlossySurfaces, options.sharcFootprintTest};
    append(cacheSettings, sizeof(cacheSettings));
    append(&options.sharcFireflyClamp, sizeof(options.sharcFireflyClamp));
    const auto temporalEpoch = resources.temporalEpoch();
    append(&temporalEpoch, sizeof(temporalEpoch));
    lap(breakdown.lightingMilliseconds);
    configureSharc(frame, options, temporalEpoch, tables.staticBytes(), tables.staticGeneration(),
                   tables.dynamicBytes(), hasCacheReceivers);
    lap(breakdown.sharcMilliseconds);
    // Equal signatures stay interchangeable: keep the newest, whose prefix is
    // the one kept (an older prefix would otherwise stay alive with it).
    const bool sceneChanged = !signature.equal(lastScene);
    lastScene = std::move(signature);
    if (sceneChanged || history.reset())
      reconstructionWarmFrames = 0;
    // An unchanged world can reuse its cached output once both histories are warm.
    if (!sceneChanged && !profileEveryFrame && !std::getenv("REMIX_PATH_LINEAR_CAPTURE") &&
        !options.accumulate && !debugView && !postProcessing->needsGuide() && denoiser && !denoiser->statistics().redrawRequested &&
        !history.reset() && !(upscaler && reconstructionWarmFrames < 128) &&
        !(sharc.stats.active && sharc.hasReceivers && sharc.warmFrames < 2 * options.sharcAccumulationFrames))
      return;
    if (sceneChanged) {
      ++stats.sceneGeneration;
      stats.accumulationCount = 0;
    }
    lap(breakdown.signatureMilliseconds);
    if (options.accumulate != bool(stats.accumulationEnabled))
      stats.accumulationCount = 0;
    if (!options.accumulate || stats.accumulationCount >= 1048576)
      stats.accumulationCount = 0;
    const bool localBlueNoise = (uint32_t(frame.jitter[3]) & 2u) != 0;
    const bool localSettingsChanged = frame.localScale[0] != previousLocalIntensity ||
                                      frame.localScale[1] != previousLocalRange ||
                                      localBlueNoise != previousLocalBlueNoise;
    // Only estimator changes shorten the local-light history. Lighting changes
    // (flickering, animated or switched lights) are left to NRD's per-pixel
    // anti-lag: a scene-wide reset on every flicker kept the whole screen's
    // local lighting at two accumulated frames.
    if (localSettingsChanged || (frame.localConfig[3] & 0x80000000u) != previousLocalAdaptive)
      localResponsiveFrames = 8;
    else if (localResponsiveFrames)
      --localResponsiveFrames;
    previousLocalIntensity = frame.localScale[0];
    previousLocalRange = frame.localScale[1];
    previousLocalBlueNoise = localBlueNoise;
    previousLocalAdaptive = frame.localConfig[3] & 0x80000000u;
    temporalFrame = {};
    // NRD's hit-distance scale A is in meters (3 m); B and C are unitless.
    temporalFrame.hitDistance[0] = 3.f / options.sceneScale;
    // The frame's draws, and the static set's when it is traced from its units.
    const bool staticUnits = prepared.staticUnits;
    temporalFrame.shadow[2] =
        (staticUnits && statics.livePbrSurfaces()) ||
        std::any_of(resources.draws().begin(), resources.draws().end(),
                    [](const auto &draw) { return draw.traced() && draw.material->pbr; });
    temporalFrame.shadow[3] =
        (staticUnits && statics.liveSurfaces() > statics.livePbrSurfaces()) ||
        std::any_of(resources.draws().begin(), resources.draws().end(),
                    [](const auto &draw) { return draw.traced() && !draw.material->pbr; });
    // Table entries with blending enabled (state[3] bit 1): blended geometry.
    temporalFrame.translucency[0] = denoiser && (postReblur || (rrActive() && postCapabilities.reblurShSupported)) && options.translucencyDenoiser &&
        hasBlendedGeometry;
    temporalFrame.translucency[1] = denoiser && postReblur && !rrActive() && options.translucencyReuse &&
        options.bounces > 0 && options.indirectClamp == 0 && !debugView && hasBlendedGeometry;
    temporalFrame.translucency[2] = denoiser && psrCompute && options.rasterPrimary && options.primarySurfaceReplacement;
    temporalFrame.local[0] = denoiser && postLocalDenoise;
    temporalFrame.local[1] = frame.localConfig[0];
    temporalFrame.local[2] = localResponsiveFrames;
    if (std::getenv("REMIX_EMISSIVE_DIAGNOSTICS"))
      std::fprintf(stderr,
                   "EMISSION mode=%u sources=%zu raw_delta=%.6f filtered_delta=%.6f response=%u "
                   "generation=%llu visibility=%llu\n",
                   emissionMode, emissionHistory.size(), emissionHistory.rawChange,
                   emissionHistory.filteredChange, localResponsiveFrames,
                   static_cast<unsigned long long>(lightBvh.lightingGeneration),
                   static_cast<unsigned long long>(lightBvh.visibilityGeneration));
    temporalFrame.local[3] = frame.splitLocal[0];
    for (unsigned i = 0; i < 2; ++i) {
      const auto &family = i ? emissiveBvh : torchBvh;
      if (previousFamilyGeneration[i] != family.builds || localSettingsChanged || history.reset())
        familyResponsive[i] = 8;
      else if (familyResponsive[i])
        --familyResponsive[i];
      previousFamilyGeneration[i] = family.builds;
      temporalFrame.families[i] = frame.splitLocal[i + 1];
      temporalFrame.families[i + 2] = familyResponsive[i];
    }
    if (!splitFamilies) {
      temporalFrame.families[0] = frame.localConfig[0];
      temporalFrame.families[1] = 0;
      temporalFrame.families[2] = localResponsiveFrames;
      temporalFrame.families[3] = 0;
    }
    temporalFrame.view = resources.view();
    temporalFrame.inverseCamera = frame.inverseCamera;
    pendingCamera = frame.camera;
    pendingProjection = resources.projection();
    temporalFrame.previousView = history.reset() ? resources.view() : previousView;
    temporalFrame.previousCamera = history.reset() ? frame.camera : previousCamera;
    temporalFrame.previousInverseCamera = scene::inverse(temporalFrame.previousCamera);
    auto halton = [](uint32_t n, uint32_t base) {
      float value = 0, scale = 1.f / base;
      while (n) {
        value += float(n % base) * scale;
        n /= base;
        scale /= base;
      }
      return value - .5f;
    };
    // Camera jitter needs a resolve. NRD filters indirect lighting, not primary
    // coverage; raw and NRD-only rendering must use stable pixel centers.
    if (options.accumulate || (denoiser && postTaa)) {
      temporalFrame.jitter[0] = halton(uint32_t(sequence % 16) + 1, 2);
      temporalFrame.jitter[1] = halton(uint32_t(sequence % 16) + 1, 3);
    }
    if (upscaler)
      upscale::jitter(temporalFrame.jitter, sequence, extent, displayExtent);
    std::copy_n(temporalFrame.jitter, 2, frame.jitter);
    temporalFrame.jitter[2] = history.reset() ? temporalFrame.jitter[0] : previousJitter[0];
    temporalFrame.jitter[3] = history.reset() ? temporalFrame.jitter[1] : previousJitter[1];
    temporalFrame.hitDistance[3] = rrActive() ? 1.f : 0.f;
    temporalFrame.motion[0] = options.movingHistoryStart;
    temporalFrame.motion[1] = options.movingHistoryEnd;
    temporalFrame.motion[2] = options.movingIndirectHistory;
    temporalFrame.motion[3] = options.movingLocalHistory;
    temporalFrame.motionGradient[0] = options.movingNeighborHistory;
    temporalFrame.motionGradient[1] = options.movingGradients;
    temporalFrame.motionGradient[2] = options.movingLightTolerance;
    temporalFrame.motionGradient[3] = options.sceneScale / options.movingLightScale;
    temporalFrame.motionNoise[0] = options.movingGradientNoise;
    temporalFrame.motionNoise[1] = options.taaMotionRate;
    temporalFrame.flags[0] = history.reset() || frame.sun[2] != previousSun[7];
    temporalFrame.flags[1] = postReblur;
    temporalFrame.flags[2] = postTaa;
    temporalFrame.flags[3] = denoiser ? uint32_t(denoiser->statistics().renderedFrames % 2) : 0;
    temporalFrame.target[0] = extent.width;
    temporalFrame.target[1] = extent.height;
    temporalFrame.target[2] = previousSeed;
    temporalFrame.target[3] = options.samples;
    temporalFrame.timing[0] =
        previousTime.time_since_epoch().count()
            ? std::clamp(float(std::chrono::duration<double, std::milli>(pendingTime - previousTime).count()),
                         1.f, 1000.f)
            : 16.6667f;
    temporalFrame.timing[1] = options.validation;
    temporalFrame.timing[2] = sceneChanged;
    temporalFrame.timing[3] = float(debugView);
    for (int i = 0; i < 3; ++i) {
      temporalFrame.sun[i] = -frame.direction[i];
      pendingSun[i] = frame.direction[i];
      pendingSun[i + 3] = frame.radiance[i];
    }
    pendingSun[6] = diameter;
    pendingSun[7] = frame.sun[2];
    pendingSun[8] = (uint32_t(frame.jitter[3]) & 1u) != 0;
    temporalFrame.sun[3] = frame.sun[1];
    // Abrupt sun changes invalidate only SIGMA. Ordinary time/weather changes
    // keep both denoisers' histories and the existing lighting confidence path.
    float directionDelta = 0, radianceDelta = 0, previousRadiance = 0;
    for (int i = 0; i < 3; ++i) {
      directionDelta += (pendingSun[i] - previousSun[i]) * (pendingSun[i] - previousSun[i]);
      radianceDelta += std::abs(pendingSun[i + 3] - previousSun[i + 3]);
      previousRadiance += previousSun[i + 3];
    }
    temporalFrame.shadow[0] = directionDelta > .01f ||
                              radianceDelta > std::max(.1f, previousRadiance * .5f) ||
                              diameter != previousSun[6] || pendingSun[8] != previousSun[8];
    temporalFrame.shadow[1] = uint32_t(frame.sun[2]);
    frame.lightNodes = lightNodes ? lightNodes->address : 0;
    frame.lightEmitters = lightEmitters ? lightEmitters->address : 0;
    const bool visibilityActive = localIlluminationActive && visibilityTable && visibilityNodes && visibilityLeaves;
    frame.visibilityNodes = visibilityActive ? visibilityNodes->address : 0;
    frame.visibilityLeaves = visibilityActive ? visibilityLeaves->address : 0;
    frame.visibilityTable = visibilityActive ? visibilityTable->address : 0;
    frame.visibilityConfig[0] = lightVisibility.rowWords();
    frame.visibilityConfig[2] = lightVisibility.tableNodes();
    // Lights that changed since the previous frame: the local denoiser drops
    // history where they changed the light (a turning flashlight's trail).
    {
      static const std::vector<scene::MovedLight> none;
      const auto &moved = options.localHistoryConfidence ? lightBvh.moved() : none;
      if (movedLights)
        retiredBuffers.push_back(std::move(movedLights));
      if (!moved.empty() && deviceLocalScene)
        movedLights = upload(command, moved.data(), moved.size() * sizeof(moved[0]), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      else if (!moved.empty())
        hostBuffer(movedLights, moved.data(), moved.size() * sizeof(moved[0]), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      frame.movedLights = movedLights ? movedLights->address : 0;
      frame.visibilityConfig[1] = uint32_t(moved.size());
    }
    // IES profiles: uploaded when one is added (they are never changed).
    if (const auto ies = resources.iesProfiles(); ies != iesUploaded) {
      if (iesBuffer)
        retiredBuffers.push_back(std::move(iesBuffer));
      if (ies && !ies->empty()) {
        if (deviceLocalScene)
          iesBuffer = upload(command, ies->data(), ies->size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        else {
          std::unique_ptr<Buffer> host;
          hostBuffer(host, ies->data(), ies->size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
          iesBuffer = std::move(host);
        }
      }
      iesUploaded = ies;
    }
    frame.iesProfiles = iesBuffer ? iesBuffer->address : 0;
    // Classify the empty space just above a shading point (2 cm).
    frame.visibilityProbe[0] = .02f / options.sceneScale;
    // Paint lies at most this far above its receiver (5 cm; Source lifts 0.1 in).
    frame.visibilityProbe[1] = .05f / options.sceneScale;
    // Texture footprints are measured in output pixels: an upscaled frame
    // samples finer mips (render/output size, the usual upscaler mip bias).
    frame.visibilityProbe[2] = upscaler && displayExtent.height && extent.height < displayExtent.height
                                   ? float(extent.height) / float(displayExtent.height)
                                   : 1.f;
    frame.torchNodes = torchNodes ? torchNodes->address : 0;
    frame.torchEmitters = torchEmitters ? torchEmitters->address : 0;
    frame.emissiveNodes = emissiveNodes ? emissiveNodes->address : 0;
    frame.emissiveEmitters = emissiveEmitters ? emissiveEmitters->address : 0;
    configureLightGuiding(frame, options);
    configureSkyGuiding(frame, options);
    configureRestir(frame, options, extent);
    configureRegir(frame, options);
    configureOrca(frame, options);
    if (options.pbrLocalReuse) {
      const uint32_t settings[] = {options.localSamples,
                                   options.emissiveCandidates,
                                   options.sphereCandidates,
                                   uint32_t(options.adaptiveLocalSampling),
                                   uint32_t(options.localGuiding),
                                   options.separateLocalLighting,
                                   uint32_t(options.emissiveSolidAngleSampling),
                                   uint32_t(localBlueNoise)};
      reuseSignature = hashBytes(settings, sizeof(settings), reuseSignature);
    }
    configurePbrReuse(command, frame, options, reuseSignature,
                      tables.dynamicInputs().empty() && temporalFrame.local[2] == 0 && !history.reset());
    const bool querySharc = sharc.stats.active && (sharc.hasReceivers || options.sharcDebug);
    configureIndirect(frame, options, querySharc);
    {
      // The sheet lighting pass's transport.
      const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 16;
      if (!sheetResults || sheetResults->size < bytes) {
        auto next = buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        if (sheetResults) retiredBuffers.push_back(std::move(sheetResults));
        sheetResults = std::move(next);
      }
      frame.sheetResults = sheetResults->address;
    }
    configureSparse(command, frame, options, extent);
    configureVolume(command, frame, options);
    beginRayProfile(command, frame);
    frame.config[2] = uint32_t(sequence++);
    // Opt-in diagnostic offset for independently seeded HDR references.
    if (const auto *seed = std::getenv("REMIX_PATH_SAMPLE_SEED"))
      frame.config[2] += uint32_t(std::strtoul(seed, nullptr, 10));
    frame.config[3] = uint32_t(stats.accumulationCount);
    frame.firefly[2] = postProcessing->materialGuide() ? 1.f : 0.f;
    if (uniform && uniform->host == deviceLocalScene)
      uniform.reset(); // this completed slot's buffer, after a placement switch
    if (deviceLocalScene) {
      if (!uniform)
        uniform = buffer(sizeof(frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
      const VkBufferCopy copy{.srcOffset = stage(&frame, sizeof(frame)), .size = sizeof(frame)};
      vkCmdCopyBuffer(command, staging.buffer->handle, uniform->handle, 1, &copy);
    } else {
      hostBuffer(uniform, &frame, sizeof(frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    }
    // The frame constants and the moved lights staged since the table's barrier.
    if (deviceLocalScene)
      barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT);
    stats.samplesPerPixel = options.samples;
    stats.indirectBounces = options.bounces;
    stats.accumulationEnabled = options.accumulate;
    ++stats.accumulationCount;
    stats.sampleSequence = sequence;
    stats.redrawRequested =
        options.accumulate || debugView ||
        (sharc.stats.active && sharc.hasReceivers && sharc.warmFrames < 2 * options.sharcAccumulationFrames);
    lap(breakdown.traceSetupMilliseconds);
    if (textures.empty())
      textures.push_back(resources.sampledMaterial(resources.defaultMaterial(), command));
    breakdown.textureDescriptors += textures.size();
    textures.resize(PathTracer::textureLimit, textures.front());
    VkDescriptorBufferInfo buffers[] = {{table->handle, 0, tableBytes},
                                        {uniform->handle, 0, sizeof(frame)}};
    VkDescriptorImageInfo images[] = {{VK_NULL_HANDLE, world->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, transmission->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, sky->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[0]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[1]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[2]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[3]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[4]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[5]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, primary[6]->view, VK_IMAGE_LAYOUT_GENERAL},
                                      {VK_NULL_HANDLE, postProcessing->materialGuide() ? postProcessing->materialGuide() : primary[6]->view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSetAccelerationStructureKHR structures[2] = {};
    VkWriteDescriptorSet writes[16] = {};
    for (unsigned i = 0; i < 16; ++i) {
      writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                   .dstSet = set,
                   .dstBinding = i,
                   .descriptorCount = 1};
      if (i < 2) {
        structures[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                         .accelerationStructureCount = 1,
                         .pAccelerationStructures = &tlas[topCombined ? 2 : i].accel->handle};
        writes[i].pNext = &structures[i];
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      } else if (i < 4) {
        writes[i].descriptorType =
            i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &buffers[i - 2];
      } else {
        writes[i].descriptorType =
            i != 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = i >= 7 ? &images[i - 5] : i < 6 ? &images[i - 4] : textures.data();
        if (i == 6)
          writes[i].descriptorCount = PathTracer::textureLimit;
      }
    }
    vkUpdateDescriptorSets(device, 16, writes, 0, nullptr);
    if (!imagesInitialized) {
      VkImageMemoryBarrier barriers[2] = {};
      for (unsigned i = 0; i < 2; ++i)
        barriers[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                       .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                       .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                       .image = i ? transmission->handle : world->handle,
                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 2, barriers);
      imagesInitialized = true;
    } else
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    lap(breakdown.descriptorMilliseconds);
    VkClearValue clear[2] = {};
    clear[0].color.float32[0] = .025f;
    clear[0].color.float32[1] = .035f;
    clear[0].color.float32[2] = .055f;
    clear[0].color.float32[3] = 1;
    // With a dome, the rasterized sky layer (3D skybox) is drawn over it:
    // alpha -1 marks pixels no sky geometry covered (any draw writes >= 0).
    if (options.sky && resources.dome().enabled)
      clear[0].color.float32[3] = -1;
    clear[1].depthStencil.depth = 1;
    VkRenderPassBeginInfo skyBegin{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                   .renderPass = skyPass,
                                   .framebuffer = skyFramebuffer,
                                   .renderArea = {{0, 0}, extent},
                                   .clearValueCount = 2,
                                   .pClearValues = clear};
    {
      gpu::GpuScope scope(command, "Sky raster", extent);
      vkCmdBeginRenderPass(command, &skyBegin, VK_SUBPASS_CONTENTS_INLINE);
      resources.record(command, true, false, temporalFrame.jitter, extent);
      vkCmdEndRenderPass(command);
    }
    if (timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, timestamps, 2);
    {
      gpu::GpuScope scope(command, "Light guiding clear");
      beginLightGuiding(command);
      beginSkyGuiding(command);
    }
    auto stageTime = [&](uint32_t query) {
      if (timestamps)
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, timestamps, query);
    };
    lap(breakdown.passesMilliseconds);
    recordPrimary(command, options.rasterPrimary);
    lap(breakdown.primaryMilliseconds);
    stageTime(4);
    recordRegir(command);
    recordVolume(command);
    recordSharc(command);
    recordOrca(command, extent);
    recordSparse(command, extent);
    if (denoiser) {
      denoiser->setLocalTuning({options.localAntilagSigma, options.localAntilagSensitivity, options.localHistoryFrames,
                                options.indirectHistoryFrames});
      denoiser->begin(command, temporalFrame, pendingProjection,
                      history.reset() ? pendingProjection : previousProjection, world->view, sky->view,
                      options.localFireflyFilter && (postLocalDenoise || options.bounces > 0));
    }
    stageTime(5);
    // One lighting family (lightingStage): the cache query is gated per
    // frame by sharcConfig, so an idle cache costs no pipeline of its own.
    const auto opaqueSource = denoiser ? temporalCompute : compute;
    VkPipeline opaquePipeline = VK_NULL_HANDLE;
    if (indirectPass.active) {
      opaquePipeline = selectLightingPipeline(opaqueSource, false, true);
      if (!opaquePipeline) {
        indirectPass.active = false;
        // Retry the optional consumer on later frames while it compiles.
        if (indirectPass.results)
          retiredBuffers.push_back(std::move(indirectPass.results));
        std::fprintf(stderr, "Separate indirect consumer unavailable; using inline tracing\n");
      }
    }
    if (!opaquePipeline)
      opaquePipeline = selectLightingPipeline(opaqueSource);
    const auto opaqueKey = selectedLightingKey;
    const auto opaqueFeatures = selectedFeatures;
    const auto opaqueLayout = selectedLayout;
    // The consumer base pipeline without its producer this frame (the pass
    // failed, or its inline variant is still compiling): black indirect light.
    const auto opaqueEntry = lightingPipelines.find(opaqueSource);
    const bool consumerAlone = !indirectPass.active && opaquePipeline == opaqueSource && indirectPass.results &&
                               opaqueEntry != lightingPipelines.end() && opaqueEntry->second.baseSeparateIndirect;
    if (consumerAlone) {
      vkCmdFillBuffer(command, indirectPass.results->handle, 0, VK_WHOLE_SIZE, 0);
      barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
    // Sparse rendering launches the opaque pass twice. Both variants compile
    // lazily; until they have, the frame is lit densely.
    VkPipeline sparseLit = VK_NULL_HANDLE, sparseGuides = VK_NULL_HANDLE;
    uint32_t sparseLitFeatures = defaultFeatures, sparseGuidesFeatures = defaultFeatures;
    if (sparse.active && !indirectPass.active) {
      sparseLit = selectLightingPipeline(opaqueSource, false, false, 1);
      sparseLitFeatures = selectedFeatures;
      sparseGuides = selectLightingPipeline(opaqueSource, false, false, 2);
      sparseGuidesFeatures = selectedFeatures;
    }
    sparse.launched = sparseLit && sparseGuides;
    // A changed rate starts Ray Reconstruction's history over, so the image
    // settles at the new rate instead of blending two.
    if (const float rate = sparse.launched ? sparse.rate : 1; rate != sparse.launchedRate) {
      sparse.launchedRate = rate;
      temporalFrame.flags[0] = 1;
    }
    trace::g_sparseState = {options.sparseRendering, sparse.launched, sparse.rate,
                            sparse.launched         ? ""
                            : sparse.inactiveReason ? sparse.inactiveReason
                                                    : "its lighting variants are compiling"};
    VkPipeline indirectPipeline = VK_NULL_HANDLE;
    uint32_t indirectKey = UINT32_MAX;
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, denoiser ? temporalLayout : layout, 0, 1,
                            &set, 0, nullptr);
    if (denoiser) {
      auto inputs = denoiser->inputs();
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, temporalLayout, 1, 1, &inputs, 0,
                              nullptr);
    }
    if (denoiser && temporalFrame.translucency[2]) {
      gpu::GpuScope scope(command, "Primary surface replacement", extent);
      bindTrace(command, psrCompute, temporalLayout, defaultFeatures);
      vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    if (denoiser && temporalFrame.translucency[0]) {
      gpu::GpuScope scope(command, "Translucency front guides", extent);
      bindTrace(command, frontGuidesCompute, temporalLayout, defaultFeatures);
      vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    if (indirectPass.active) {
      // Scratch is queue-ordered across frame slots. Publish last frame's reads
      // before overwriting, then publish this producer to the opaque consumer.
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
              VK_ACCESS_SHADER_WRITE_BIT);
      indirectPipeline = selectLightingPipeline(indirectPass.pipeline);
      indirectKey = selectedLightingKey;
      gpu::GpuScope scope(command, querySharc ? "Trace indirect (separate pass, SHARC query)"
                                              : "Trace indirect (separate pass)",
                          extent);
      bindSelected(command, indirectPipeline);
      vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
      scope.close();
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    {
      // One dispatch: surface shading, sun and local-light shadow rays,
      // emissive sampling, water and (unless separate) the indirect path.
      const char *name = indirectPass.active ? "Trace opaque: direct, sun, local, emissive"
                         : querySharc        ? "Trace opaque: direct, sun, local, emissive, indirect+SHARC"
                                             : "Trace opaque: direct, sun, local, emissive, indirect";
      if (sparse.launched) {
        // The two launches write disjoint pixels.
        {
          gpu::GpuScope scope(command, "Sparse rendering: guides of unlit pixels", extent);
          bindTrace(command, sparseGuides, opaqueLayout, sparseGuidesFeatures);
          vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
        }
        gpu::GpuScope scope(command, querySharc ? "Trace opaque, selected pixels: direct, sun, local, emissive, indirect+SHARC"
                                                : "Trace opaque, selected pixels: direct, sun, local, emissive, indirect",
                            extent);
        bindTrace(command, sparseLit, opaqueLayout, sparseLitFeatures);
        vkCmdDispatchIndirect(command, sparse.count->handle, 16);
      } else {
        gpu::GpuScope scope(command, name, extent);
        bindTrace(command, opaquePipeline, opaqueLayout, opaqueFeatures);
        vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
      }
    }
    stageTime(6);
    // Publish opaque lighting, RNG and sheet count to transparent composition.
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (restir.active) {
      recordRestir(command, extent);
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    } else if (splitLightingActive && denoiser && postSplit) {
      gpu::GpuScope scope(command, "Trace split local/emissive direct", extent);
      bindSelected(command, selectLightingPipeline(splitDirectCompute, options.separateLocalLighting == 3));
      vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
      barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    stageTime(7);
    stageTime(10);
    if (denoiser) {
      denoiser->beginConfidence(command);
      if (postReblur) {
        barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        bindSelected(command, selectLightingPipeline(confidenceCompute));
        gpu::GpuScope scope(command, "Trace indirect confidence (1/5 res)", (extent.width + 4) / 5,
                            (extent.height + 4) / 5);
        vkCmdDispatch(command, ((extent.width + 4) / 5 + 7) / 8, ((extent.height + 4) / 5 + 7) / 8, 1);
      }
      stageTime(8);
      denoiser->recordOpaque(command);
      // NRD binds its own sets and layout. Restore both tracer sets before
      // reading the opaque result or writing foreground signals.
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, temporalLayout, 0, 1, &set, 0, nullptr);
      auto inputs = denoiser->inputs();
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, temporalLayout, 1, 1, &inputs, 0, nullptr);
    } else {
      stageTime(8);
    }
    stageTime(11);
    // The multi-sheet pass compiles off the activation batch; until it is
    // ready the composition pass composes layered pixels from the front
    // sheet's light (REMIX_MULTI_SHEET_FALLBACK=1 keeps it so, for comparison).
    // With layer reuse that is the multi-sheet pass's own result (deeper
    // layers take the front one's light) at a fraction of its cost: the sheet
    // lighting and composition passes are smaller stages (a screen of
    // two-layer glass at 1129x635 on an RTX 2080: 18.8 ms -> 3.4 + 1.8 ms).
    static const bool fallbackRequested = [] {
      const auto *value = std::getenv("REMIX_MULTI_SHEET_FALLBACK");
      return value && std::strcmp(value, "1") == 0;
    }();
    const bool forceFallback = fallbackRequested || options.layerReuse;
    if (denoiser && !temporalTransparencyCompute && !multiSheetFailed && !forceFallback) {
      try {
        createLightingPipelines({{lightingStage(trace::Stage::temporalTransparency), temporalLayout,
                                  &temporalTransparencyCompute, false}});
      } catch (VkResult error) {
        if (error == VK_ERROR_DEVICE_LOST) throw;
        if (error != VK_NOT_READY) {
          multiSheetFailed = true;
          std::fprintf(stderr, "Multi-sheet pass unavailable (%d); composing layered pixels from the front sheet\n", int(error));
        }
      }
    }
    const auto multiSheetPipeline = forceFallback ? VK_NULL_HANDLE : denoiser ? temporalTransparencyCompute : transparencyCompute;
    {
      // The front sheet's light (direct, paths, reused GI), for the
      // composition below; this frame's opaque radiance and NRD outputs are
      // available to its reuse. Layered pixels take it only when the
      // composition pass composes them (the multi-sheet pass lights its own
      // front sheet).
      gpu::GpuScope scope(command, "Sheet lighting (front sheet)", extent);
      const auto sheetPipeline = selectLightingPipeline(denoiser ? sheetTemporalCompute : sheetCompute);
      if (!multiSheetPipeline)
        selectedFeatures |= featureMultiSheetFallback;
      bindSelected(command, sheetPipeline);
      vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    }
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const auto singleTransparencyPipeline =
        selectLightingPipeline(denoiser ? singleTemporalTransparencyCompute : singleTransparencyCompute);
    if (!multiSheetPipeline)
      selectedFeatures |= featureMultiSheetFallback;
    {
      gpu::GpuScope scope(command, "Trace transparency (single sheet)", extent);
      bindSelected(command, singleTransparencyPipeline);
      vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    }
    // Single-sheet composition replaces its scratch count with stable depth (0..1).
    // The batch shader only visits counts > 1, so these pixels remain untouched.
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const auto transparencyPipeline = multiSheetPipeline ? selectLightingPipeline(multiSheetPipeline) : VK_NULL_HANDLE;
    const auto transparencyKey = selectedLightingKey;
    if (transparencyPipeline) {
      gpu::GpuScope scope(command, "Trace transparency (multi-sheet)", extent);
      bindSelected(command, transparencyPipeline);
      vkCmdDispatch(command, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    }
    if (prewarmPending) {
      // Behind this frame's own requests: those get the workers first.
      prewarmPending = false;
      for (auto &[source, entry] : lightingPipelines)
        if (entry.variants)
          prewarmLightingVariants(entry);
    }
    if (timestamps) {
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, timestamps, 3);
      timingPending = true;
    }
    if (denoiser)
      denoiser->record(command);
    stageTime(9);
    if (stateProfile) {
      const uint64_t values[] = {frame.config[2],
                                 frameIndex,
                                 uint64_t(querySharc),
                                 uint64_t(deviceLocalScene),
                                 uint64_t(indirectPass.active),
                                 uint64_t(opaquePipeline),
                                 opaqueKey,
                                 uint64_t(indirectPipeline),
                                 indirectKey,
                                 uint64_t(transparencyPipeline),
                                 transparencyKey,
                                 tlas[0].accel ? tlas[0].accel->address : 0,
                                 tlas[1].accel ? tlas[1].accel->address : 0,
                                 table->address,
                                 uint64_t(table->memory),
                                 table->size,
                                 uniform->address,
                                 uint64_t(uniform->memory),
                                 frame.lightNodes,
                                 frame.lightEmitters,
                                 frame.sharcHash,
                                 frame.sharcAccumulation,
                                 frame.sharcResolved,
                                 frame.indirectResults,
                                 stats.sceneGeneration,
                                 stats.staticTlasBuilds,
                                 hashBytes(tailData, tableBytes - prefixBytes, hashBytes(prefixData, prefixBytes))};
      traceState.clear();
      for (auto value : values) {
        traceState += std::to_string(value);
        traceState += ',';
      }
      constexpr char hex[] = "0123456789abcdef";
      for (size_t i = 0; i < sizeof(frame); ++i) {
        const auto value = reinterpret_cast<const uint8_t *>(&frame)[i];
        traceState += hex[value >> 4];
        traceState += hex[value & 15];
      }
    }
    frameGeneration = {};
    if (upscaler) {
      upscaler->record(command, denoiser->image(denoise::Composed), denoiser->image(denoise::Depth),
                       denoiser->image(denoise::Motion), denoiser->image(denoise::Transmission),
                       pendingProjection, temporalFrame.jitter, temporalFrame.timing[0], options.fsrSharpness,
                       temporalFrame.flags[0],
                       {denoiser->image(denoise::RrDiffuse), denoiser->image(denoise::RrSpecular),
                        denoiser->image(denoise::RrNormal), denoiser->image(denoise::RrHitDistance)},
                       resources.view());
      // Frame generation (runtime/frame_generation.h) interpolates the
      // finished frame with the depth and motion the upscaler just prepared.
      frameGeneration.images = upscaler->frameGenerationInputs();
      frameGeneration.view = resources.view();
      frameGeneration.projection = pendingProjection;
      frameGeneration.previousView = history.reset() ? resources.view() : previousView;
      frameGeneration.previousProjection = history.reset() ? pendingProjection : previousProjection;
      frameGeneration.frameMilliseconds = temporalFrame.timing[0];
      frameGeneration.reset = temporalFrame.flags[0] != 0;
      frameGeneration.valid = frameGeneration.images.valid && !debugView;
    }
    captureLinear(command);
    finishPbrReuse(command);
    finishRayProfile(command);
    finishLightGuiding(command);
    finishSharc(command);
    rendered = true;
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    stats.accelerationBytes = 0;
    for (const auto &arena : arenas)
      stats.accelerationBytes += arena->buffer->size;
    if (scratch)
      stats.accelerationBytes += scratch->size;
    if (topScratch)
      stats.accelerationBytes += topScratch->size;
    for (const auto &top : tlas)
      if (top.instances)
        stats.accelerationBytes += top.instances->size;
    stats.peakAccelerationBytes = std::max(stats.accelerationBytes, stats.peakAccelerationBytes);
  }

  scene::TraceFrame PathTracer::prepareScene(RasterRenderer &resources, const TraceOptions &options) const {
    using Clock = std::chrono::steady_clock;
    auto &g = *m_gpu;
    const char *profile = std::getenv("REMIX_OPACITY_PROFILE");
    const auto start = Clock::now();
    if (!g.workers)
      g.workers = std::make_unique<WorkerPool>();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.epoch).count();
    const bool filterEmission = options.emissiveLights && options.localLights &&
                                (options.freezeEmissiveRadiance || options.stabilizeEmissiveLighting);
    // The persistent static set stays in the tracer's units (updated from the
    // handles that changed); the frame's other draws are prepared as before:
    // coverage proof is CPU-only and can overlap the previous GPU submission,
    // and cached partitions are found on workers, the rest proved in draw
    // order. The opacity profile's lookup and hit counters need every lookup
    // in partition(): it traces the static set as draws.
    g.statics.resetStatistics();
    // Hardware face culling changes one-sided coverage: the kept static set
    // proves its partitions again (prepare() reads the flag set here).
    if (options.hardwareFaceCulling != g.opacityCache.oneSidedOpaqueEnabled()) {
      g.opacityCache.setOneSidedOpaque(options.hardwareFaceCulling);
      g.statics.invalidate();
    }
    // View models move into the world camera's view (Draw::viewModel) before
    // the tracer and the raster overlays see the frame's draws.
    if (options.viewModel)
      resources.applyViewModel(options.viewModelScale);
    Clock::time_point prepared{};
    scene::TraceFrame frame;
    if (profile) {
      resources.expandStatic();
      frame = scene::prepareTraceFrame(resources.draws(), seconds, filterEmission, g.surfaceEmissionCache);
      prepared = Clock::now();
      for (auto instance : frame.instances)
        for (auto &surface : instance)
          surface.opacity = g.opacityCache.partition(surface.draw, surface.uv);
    } else {
      frame = scene::prepareSceneFrame(resources, &g.statics, g.opacityCache, g.workers.get(), false, seconds,
                                       filterEmission, g.surfaceEmissionCache);
    }
    const auto classified = Clock::now();
    m_gpu->opacityCache.prune();
    const auto pruned = Clock::now();
    if (std::getenv("REMIX_OPACITY_DIAGNOSTICS")) {
      uint64_t reasons[scene::OpacityPartition::Count] = {}, mixed = 0;
      auto count = [&](const scene::TraceFrame::Surface &surface) {
        const auto &partition = surface.opacity;
        mixed += partition->key[0] == 2;
        for (unsigned i = 0; i < scene::OpacityPartition::Count; ++i)
          reasons[i] += partition->reasons[i];
      };
      for (const auto &instance : frame.instances)
        for (const auto &surface : instance)
          count(surface);
      if (frame.staticUnits)
        for (const uint32_t slot : g.statics.order())
          if (!g.statics.units()[slot].singular)
            for (const auto &surface : g.statics.units()[slot].surfaces)
              count(surface);
      std::fprintf(stderr, "OPACITY mixed_surfaces=%llu", static_cast<unsigned long long>(mixed));
      const char *names[] = {"solid",   "one_sided",       "blended",          "transmission",  "alpha_test",
                             "invalid", "missing_texture", "texture_boundary", "texture_cutout"};
      for (unsigned i = 0; i < scene::OpacityPartition::Count; ++i)
        std::fprintf(stderr, " %s=%llu", names[i], static_cast<unsigned long long>(reasons[i]));
      std::fprintf(stderr, "\n");
    }
    const auto stageStart = Clock::now();
    auto &staged = frame.lighting;
    staged.combined = std::move(m_gpu->lightBvh);
    staged.history = std::move(m_gpu->emissionHistory);
    staged.visibility = std::move(m_gpu->lightVisibility);
    staged.staged = true;
    const auto mode = options.freezeEmissiveRadiance ? 2u : options.stabilizeEmissiveLighting ? 1u : 0u;
    staged.history.begin(resources.temporalEpoch().world, frame.seconds,
                         options.emissiveLights && options.localLights ? mode : 0u);
    const auto lightStart = Clock::now();
    const auto stageEnd = lightStart;
    // Storage kept from frame to frame (megabytes with many emissive triangles).
    auto &lights = m_gpu->lightInput;
    lights.clear();
    if (options.torchLights) {
      const auto spheres = resources.sphereLights();
      lights.assign(spheres.begin(), spheres.end());
    }
    const auto sphereCount = lights.size();
    // The static set's emitters where the set keeps them (scene::mergedEmitters
    // has them in trace order; the light tree orders its input itself), then
    // the frame's own.
    size_t emitterCount = frame.emitterCount;
    if (frame.staticUnits)
      for (const uint32_t slot : g.statics.emitting())
        emitterCount += g.statics.units()[slot].emitters.size();
    lights.reserve(sphereCount + emitterCount);
    const auto gather = [&](const scene::TraceFrame::Emitter &source, bool animated) {
      auto light = source.light;
      if (animated)
        staged.history.apply(light, source.layout);
      if (options.emissiveLights && light.radius > 1e-8f)
        lights.push_back(light);
    };
    if (frame.staticUnits)
      for (const uint32_t slot : g.statics.emitting())
        for (const auto &source : g.statics.units()[slot].emitters)
          gather(source, filterEmission && source.animated);
    // The frame's runs: each light copied once, into the tree's input.
    for (const auto &run : frame.emitters) {
      const auto &source = *run.lights;
      for (size_t i = 0; i < source.size(); ++i) {
        lights.push_back(source[i]);
        auto &light = lights.back();
        light.dynamic = run.dynamic;
        if (run.animated)
          staged.history.apply(light, (*run.reduction)[i].layout);
        if (!options.emissiveLights || !(light.radius > 1e-8f))
          lights.pop_back();
      }
    }
    staged.history.end();
    staged.hasEmissive = lights.size() > sphereCount;
    const auto gathered = Clock::now();
    const auto lightCount = uint32_t(lights.size());
    // Per-family trees also for torch-only scenes: a split denoiser needs them.
    if (options.separateLocalLighting && options.separateLocalLighting != 3) {
      staged.torch = std::move(m_gpu->torchBvh);
      staged.emissive = std::move(m_gpu->emissiveBvh);
      staged.families = true;
      staged.emissive.setClusterSize(options.emissiveClusterSize);
      staged.torch.update({lights.begin(), lights.begin() + sphereCount});
      staged.emissive.update({lights.begin() + sphereCount, lights.end()});
    }
    const auto split = Clock::now();
    staged.combined.setClusterSize(options.emissiveClusterSize);
    staged.combined.update(lights);
    {
      std::lock_guard lock(scene::g_lightTreeSaveMutex);
      if (!scene::g_lightTreeSavePath.empty()) {
        staged.combined.save(scene::g_lightTreeSavePath.c_str());
        scene::g_lightTreeSavePath.clear();
      }
    }
    const auto treed = Clock::now();
    staged.visibility.update(resources.visibilityPartition(), staged.combined,
                             options.lightVisibilityCulling && options.localLights, .1f / options.sceneScale);
    {
      const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
      const auto end = Clock::now();
      const trace::SceneCosts costs{ms(start, end),         ms(start, classified), ms(classified, pruned),
                                    ms(stageStart, stageEnd), ms(stageEnd, gathered), ms(gathered, split),
                                    ms(split, treed),         ms(treed, end)};
      auto &totals = trace::g_sceneCosts;
      auto &sum = totals.sum;
      sum.total += costs.total, sum.frame += costs.frame, sum.prune += costs.prune, sum.stage += costs.stage;
      sum.gather += costs.gather, sum.split += costs.split, sum.tree += costs.tree, sum.visibility += costs.visibility;
      ++totals.frames;
      if (costs.total > totals.slowest.total) {
        totals.slowest = costs;
        totals.lights = lightCount;
      }
    }
    staged.emittingInstances = frame.emittingInstances;
    staged.dynamicEmittingInstances = frame.dynamicEmittingInstances;
    staged.updateMilliseconds = std::chrono::duration<double, std::milli>(Clock::now() - lightStart).count();
    if (profile) {
      const auto end = Clock::now();
      const auto count = m_gpu->opacityCache.takeStatistics();
      if (auto *file = std::fopen(profile, "a")) {
        if (!std::ftell(file))
          std::fprintf(file, "prepare_ms,opacity_ms,prune_ms,lights_ms,lookups,hits,proofs,triangles,"
                             "evictions,owners,textures\n");
        auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        std::fprintf(file, "%.6f,%.6f,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n", ms(start, prepared),
                     ms(prepared, classified), ms(classified, pruned), ms(pruned, end),
                     (unsigned long long)count.lookups, (unsigned long long)count.hits,
                     (unsigned long long)count.proofs, (unsigned long long)count.triangles,
                     (unsigned long long)count.evictions, (unsigned long long)count.owners,
                     (unsigned long long)count.textures);
        std::fclose(file);
      }
    }
    return frame;
  }

  void PathTracer::returnScene(scene::TraceFrame &frame) const {
    auto &lighting = frame.lighting;
    if (!lighting.staged)
      return;
    auto &g = *m_gpu;
    g.lightBvh = std::move(lighting.combined);
    g.emissionHistory = std::move(lighting.history);
    g.lightVisibility = std::move(lighting.visibility);
    if (lighting.families) {
      g.torchBvh = std::move(lighting.torch);
      g.emissiveBvh = std::move(lighting.emissive);
    }
    lighting.staged = false;
  }

  // The frame's mesh views, in draw order: pass 0 finds the views whose lookup
  // changes nothing (unstreamed and up to date) on workers, together with the
  // opacity partitions; pass 1 looks up the rest serially (new views,
  // conversions and streamed lineages, whose transfers depend on order); the
  // CPU halves of the conversions (skinning, BLAS input assembly and hashing)
  // run on workers, and the uploads follow in draw order.
  void PathTracer::Gpu::TableViews::resolveViews(const std::vector<Span> &instances,
                                                 std::vector<const MeshView *> &views) {
    auto &g = *gpu;
    const size_t count = instances.size();
    size_t partitionTotal = 0;
    g.partitionOffsets.resize(count);
    for (size_t index = 0; index < count; ++index) {
      g.partitionOffsets[index] = partitionTotal;
      partitionTotal += instances[index][0].draw.mesh->surfaces.size();
    }
    g.partitionStorage.assign(partitionTotal, nullptr); // Stable: conversions keep pointers.
    g.instanceViews.resize(count);
    {
      ScopedTicks time{g.geometryTicks};
      constexpr size_t instancesPerTask = 512;
      g.workers->run((count + instancesPerTask - 1) / instancesPerTask, [&](size_t task) {
        for (size_t index = task * instancesPerTask; index < std::min(count, (task + 1) * instancesPerTask);
             ++index) {
          const auto instance = instances[index];
          auto *partitions = g.partitionStorage.data() + g.partitionOffsets[index];
          for (const auto &preparedSurface : instance)
            partitions[preparedSurface.draw.surfaceIndex] = preparedSurface.opacity.get();
          const auto &draw = instance[0].draw;
          g.instanceViews[index] = {g.findView(draw, partitions, draw.mesh->surfaces.size()), -1};
        }
      });
    }
    size_t pendingConversions = 0;
    for (size_t index = 0; index < count; ++index) {
      if (g.instanceViews[index].view)
        continue;
      const auto &draw = instances[index][0].draw;
      if (pendingConversions == g.conversions.size())
        g.conversions.emplace_back();
      bool convert = false;
      auto &view = g.meshView(draw, g.partitionStorage.data() + g.partitionOffsets[index],
                              draw.mesh->surfaces.size(), g.conversions[pendingConversions], convert);
      g.instanceViews[index] = {&view, convert ? int32_t(pendingConversions++) : -1};
    }
    if (pendingConversions) {
      ScopedTicks time{g.geometryTicks};
      g.workers->run(pendingConversions, [&](size_t i) { convertMesh(g.conversions[i]); });
    }
    views.resize(count);
    for (size_t index = 0; index < count; ++index) {
      if (g.instanceViews[index].conversion >= 0)
        g.commitMesh(command, g.conversions[g.instanceViews[index].conversion]);
      views[index] = g.instanceViews[index].view;
    }
  }

  bool PathTracer::Gpu::TableViews::findViews(const std::vector<Span> &instances,
                                              std::vector<const MeshView *> &views) {
    auto &g = *gpu;
    views.resize(instances.size());
    std::vector<const scene::OpacityPartition *> partitions;
    for (size_t index = 0; index < instances.size(); ++index) {
      const auto instance = instances[index];
      const auto &draw = instance[0].draw;
      partitions.assign(draw.mesh->surfaces.size(), nullptr);
      for (const auto &preparedSurface : instance)
        partitions[preparedSurface.draw.surfaceIndex] = preparedSurface.opacity.get();
      views[index] = g.findView(draw, partitions.data(), partitions.size());
      if (!views[index])
        return false;
    }
    return true;
  }

  PathTracer::StaticStatistics PathTracer::staticStatistics() const {
    auto out = m_gpu->staticStatistics;
    const auto &sync = m_gpu->statics.statistics();
    out.changes = sync.changes;
    out.syncMilliseconds = sync.milliseconds;
    return out;
  }

  void PathTracer::setStaticValidation(bool enabled) {
    m_gpu->staticValidation = enabled;
    if (!enabled)
      m_gpu->staticMismatchLogged = false;
  }

  void PathTracer::prepare(VkCommandBuffer command, RasterRenderer &resources, const TraceOptions &options,
                           scene::TraceFrame &scene) {
    auto &g = *m_gpu;
    measured(g.breakdown.postMilliseconds, [&] { g.postProcessing->begin(command); });
    g.prepare(command, resources, options, scene);
    ScopedMilliseconds post{g.breakdown.postMilliseconds};
    const auto hdr = g.upscaler && !g.debugView ? g.upscaler->output() : upscale::Input{g.world->handle, g.world->view};
    g.postProcessing->record(command, hdr, options, resources.temporalEpoch(), g.rendered,
      (g.postProcessing == g.postFallback.get() && !g.retainPostExposure) || options.debugView || options.shadowDebug || options.sharcDebug || options.validation, -1, g.stats.sceneGeneration);
  }

  void PathTracer::record(VkCommandBuffer command) {
    auto &g = *m_gpu;
    if (g.upscaler && !g.debugView) {
      g.upscaler->composite(command, g.encodeOutput, g.prism, g.gamma22, g.hardwareSrgb, g.postProcessing->bindings());
      return;
    }
    VkViewport viewport = {0, 0, float(g.displayExtent.width), float(g.displayExtent.height), 0, 1};
    VkRect2D scissor = {{0, 0}, g.displayExtent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &g.set, 0, nullptr);
    const auto postSet = g.postProcessing->bindings();
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 1, 1, &postSet, 0, nullptr);
    for (uint32_t i = 0; i < 2; ++i) {
      const uint32_t push[] = {i,
                               uint32_t(g.encodeOutput),
                               uint32_t(g.prism),
                               uint32_t(g.gamma22),
                               uint32_t(g.hardwareSrgb),
                               g.displayExtent.width,
                               g.displayExtent.height};
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.composite[i]);
      vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
      vkCmdDraw(command, 3, 1, 0, 0);
    }
  }

} // namespace remix
