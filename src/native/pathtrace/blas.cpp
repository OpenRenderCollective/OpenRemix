#include "pathtrace/gpu.h"

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  void PathTracer::Gpu::complete(VkCommandBuffer command) {
    completeLinearCapture();
    completeLightGuiding();
    completePbrReuse();
    completeRayProfile();
    completePathInspector();
    uploads.clear();
    staging.used = 0;
    launchSpares();
    const bool geometryReleased = !retiredGeometry.empty();
    primaryRecording.reset = false; // This slot's secondaries have completed.
    retiredGeometry.clear();
    retiredBuffers.clear();
    retiredStates.clear();
    retired.clear();
    bufferCache->trimMemory(bufferMemory);
    // Empty blocks go, but one of each memory type stays: the next buffer of
    // that type would otherwise allocate one again.
    {
      std::vector<uint32_t> kept;
      std::erase_if(bufferMemory, [&](const auto &block) {
        if (block.use_count() != 1)
          return false;
        if (std::find(kept.begin(), kept.end(), block->type) == kept.end()) {
          kept.push_back(block->type);
          return false;
        }
        return true;
      });
    }
    if (timingPending) {
      uint64_t times[12] = {};
      if (vkGetQueryPoolResults(device, timestamps, 0, 12, sizeof(times), times, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        const uint64_t mask = timestampBits >= 64 ? UINT64_MAX : (uint64_t(1) << timestampBits) - 1;
        stats.gpuBuildMilliseconds =
            float((times[1] - times[0]) & mask) * properties.limits.timestampPeriod / 1e6f;
        stats.gpuTraceMilliseconds =
            float(((times[3] - times[2]) & mask) - ((times[11] - times[10]) & mask)) * properties.limits.timestampPeriod / 1e6f;
        stats.gpuTimingsAvailable = 1;
        if (stageProfile) {
          if (stateProfile && !traceState.empty()) {
            if (auto *file = std::fopen(stateProfile, "a")) {
              if (std::ftell(file) == 0)
                std::fprintf(file, "time_ms,opaque_ms,sequence,slot,sharc,scene_device_local,split,opaque,opaque_key,producer,producer_key,transparency,transparency_key,tlas_static,tlas_dynamic,table,table_memory,table_bytes,uniform,uniform_memory,light_nodes,light_emitters,cache_hash,cache_accumulation,cache_resolved,indirect_results,scene_generation,static_builds,table_hash,frame_hex\n");
              const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
              std::fprintf(file, "%lld,%.6f,%s\n", static_cast<long long>(now),
                  double((times[6] - times[5]) & mask) * properties.limits.timestampPeriod / 1e6, traceState.c_str());
              std::fclose(file);
            }
          }
          if (auto *file = std::fopen(stageProfile, "a")) {
            if (std::ftell(file) == 0)
              std::fprintf(file, "time_ms,primary_ms,sharc_ms,opaque_ms,local_ms,transparency_ms,confidence_ms,denoise_ms,prepare_ms\n");
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto elapsed = [&](unsigned first, unsigned last) {
              return double((times[last] - times[first]) & mask) * properties.limits.timestampPeriod / 1e6;
            };
            std::fprintf(file, "%lld,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", static_cast<long long>(now),
                elapsed(2, 4), elapsed(4, 5), elapsed(5, 6), elapsed(6, 7), elapsed(11, 3), elapsed(10, 8), elapsed(8, 11) + elapsed(3, 9), elapsed(1, 2));
            std::fclose(file);
          }
        }
      }
      timingPending = false;
    }
    if (!compactPending.empty()) {
      std::vector<uint64_t> sizes(compactPending.size());
      check(vkGetQueryPoolResults(device, compaction, 0, uint32_t(sizes.size()), sizes.size() * 8,
                                  sizes.data(), 8, VK_QUERY_RESULT_64_BIT));
      for (size_t i = 0; i < sizes.size(); ++i) {
        auto &geometry = *compactPending[i];
        if (!sizes[i] || sizes[i] >= geometry.accel->size)
          continue;
        auto compact = acceleration(sizes[i], VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        VkCopyAccelerationStructureInfoKHR copy = {.sType =
                                                       VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                                                   .src = geometry.accel->handle,
                                                   .dst = compact->handle,
                                                   .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR};
        copyAs(command, &copy);
        // The mesh may disappear in this frame (including a callback's empty
        // redraw). Both copy endpoints remain referenced by this command buffer.
        retired.push_back(compact);
        retired.push_back(std::move(geometry.accel));
        geometry.accel = std::move(compact);
        tables.compacted(&geometry); // Kept TLAS inputs follow the new address.
        ++stats.blasCompactions;
      }
      compactPending.clear();
    }
    // Static geometry is released only through retiredGeometry (just cleared
    // above); skip the scan of the whole cache when nothing was retired.
    if (geometryReleased) {
      for (auto it = geometryCache.begin(); it != geometryCache.end();) {
        if (it->second.expired())
          it = geometryCache.erase(it);
        else
          ++it;
      }
    }
    // Release empty arena blocks after the last-use fence; retain one for reuse.
    for (auto it = arenas.begin(); it != arenas.end() && arenas.size() > 1;) {
      if (it->use_count() == 1)
        it = arenas.erase(it);
      else
        ++it;
    }
  }

  void PathTracer::Gpu::buildBottom(VkCommandBuffer command) {
    ScopedMilliseconds time{performance.accelerationMilliseconds};
    ScopedMilliseconds whole{trace::g_accelNow.bottom};
    std::vector<std::shared_ptr<Geometry>> work;
    VkDeviceSize scratchBytes = 0;
    for (auto &[key, view] : meshes)
      if (view.geometry->pending) {
        // Geometry can be shared by multiple material variants.
        if (std::find(work.begin(), work.end(), view.geometry) == work.end())
          work.push_back(view.geometry);
      }
    if (work.empty())
      return;
    for (const auto &geometry : work)
      scratchBytes +=
          align(geometry->update ? geometry->sizes.updateScratchSize : geometry->sizes.buildScratchSize,
                asProperties.minAccelerationStructureScratchOffsetAlignment);
    if (!scratch ||
        scratch->size < scratchBytes + asProperties.minAccelerationStructureScratchOffsetAlignment)
      scratch = buffer(scratchBytes + asProperties.minAccelerationStructureScratchOffsetAlignment,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDeviceAddress address =
        align(scratch->address, asProperties.minAccelerationStructureScratchOffsetAlignment);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> ranges;
    for (auto &g : work) {
      builds.push_back({.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                        .flags = flags(g->deform),
                        .mode = g->update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                          : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                        .srcAccelerationStructure = g->update ? g->accel->handle : VK_NULL_HANDLE,
                        .dstAccelerationStructure = g->accel->handle,
                        .geometryCount = uint32_t(g->geometries.size()),
                        .pGeometries = g->geometries.data(),
                        .scratchData = {.deviceAddress = address}});
      ranges.push_back(g->ranges.data());
      address += align(g->update ? g->sizes.updateScratchSize : g->sizes.buildScratchSize,
                       asProperties.minAccelerationStructureScratchOffsetAlignment);
      if (g->update)
        ++stats.blasRefits;
      else
        ++stats.blasBuilds;
      if (!g->deform)
        compactPending.push_back(g);
      g->pending = false;
    }
    trace::g_accelNow.bottomBuilds += uint32_t(builds.size());
    {
      ScopedMilliseconds record{trace::g_accelNow.record};
      buildAs(command, uint32_t(builds.size()), builds.data(), ranges.data());
    }
    barrier(command, buildStage, asWrite, buildStage, asRead);
    if (!compactPending.empty()) {
      ScopedMilliseconds pool{trace::g_accelNow.queryPool};
      // The pool is kept and only grown: creating one a frame with builds
      // waited behind the driver's other memory work (16 ms seen).
      if (compactionCapacity < compactPending.size()) {
        if (compaction)
          vkDestroyQueryPool(device, compaction, nullptr);
        compaction = VK_NULL_HANDLE;
        compactionCapacity = 0;
        const uint32_t capacity = std::max<uint32_t>(256, uint32_t(compactPending.size()) * 2);
        VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                      .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                      .queryCount = capacity};
        check(vkCreateQueryPool(device, &info, nullptr, &compaction));
        compactionCapacity = capacity;
      }
      vkCmdResetQueryPool(command, compaction, 0, uint32_t(compactPending.size()));
      std::vector<VkAccelerationStructureKHR> handles;
      for (const auto &g : compactPending)
        handles.push_back(g->accel->handle);
      writeAsProperties(command, uint32_t(handles.size()), handles.data(),
                        VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, compaction, 0);
    }
  }

} // namespace remix
