#include "pathtrace/gpu.h"

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  static_assert(trace::instanceFacingCullDisable == VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR &&
                trace::instanceFrontCounterClockwise == VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR);

  namespace {
    VkAccelerationStructureInstanceKHR instanceData(const trace::InstanceInput &input, bool faceCulling,
                                                    uint8_t mask = trace::allInstancesMask) {
      VkAccelerationStructureInstanceKHR instance = {};
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
          instance.transform.matrix[r][c] = input.model[c * 4 + r];
      instance.instanceCustomIndex = input.customIndex;
      instance.mask = input.viewModel        ? trace::viewModelInstanceMask
                      : input.reflectionOnly ? trace::reflectionOnlyInstanceMask
                      : input.blended        ? trace::blendedInstanceMask
                      : input.decal          ? trace::decalInstanceMask
                                             : mask;
      instance.flags = trace::instanceFlags(input, faceCulling);
      instance.accelerationStructureReference = input.geometry;
      return instance;
    }
  } // namespace

  void PathTracer::Gpu::buildTop(VkCommandBuffer command, const std::vector<trace::InstanceInput> &staticInputs,
                                 const std::vector<trace::InstanceInput> &dynamicInputs, bool force,
                                 uint64_t staticGeneration, bool faceCulling, bool combined) {
    const std::vector<trace::InstanceInput> *inputs[2] = {&staticInputs, &dynamicInputs};
    ScopedMilliseconds time{performance.accelerationMilliseconds};
    ScopedMilliseconds whole{trace::g_accelNow.top};
    // Instance flags depend on hardware face culling: rebuild when it changes.
    // A layout change starts the other layout from scratch and releases the
    // unused TLASes after this frame (their slot's retired list).
    if (faceCulling != topFaceCulling || combined != topCombined) {
      for (unsigned i = 0; i < 3; ++i) {
        tlas[i].policy = {};
        if (combined != topCombined && (i == 2) != combined && tlas[i].accel)
          retired.push_back(std::move(tlas[i].accel));
      }
      tlasStaticGeneration = 0;
      combinedPolicy = {};
      combinedStaticValid = false;
      topFaceCulling = faceCulling;
      topCombined = combined;
    }
    if (combined) {
      buildCombinedTop(command, staticInputs, dynamicInputs, force, staticGeneration);
      return;
    }
    VkAccelerationStructureGeometryKHR geometry[2] = {};
    VkAccelerationStructureBuildGeometryInfoKHR builds[2] = {};
    VkAccelerationStructureBuildSizesInfoKHR sizes[2] = {};
    uint32_t reasons[2] = {}, count[2] = {};
    bool changed[2] = {};
    VkDeviceSize scratchBytes = 0;
    for (unsigned i = 0; i < 2; ++i) {
      auto &top = tlas[i];
      // The static prefix the static TLAS already holds: nothing to compare.
      if (i == 0 && staticGeneration && staticGeneration == tlasStaticGeneration && top.policy.initialized)
        continue;
      reasons[i] = top.policy.reason(*inputs[i], i != 0, force && i != 0);
      changed[i] = reasons[i] || top.policy.changed(*inputs[i]);
      if (i == 1)
        stats.dynamicRebuildReason = changed[i] ? reasons[i] : 0;
      if (i == 0)
        tlasStaticGeneration = changed[i] ? 0 : staticGeneration;
      if (!changed[i])
        continue;
      count[i] = uint32_t(inputs[i]->size());
      if (count[i] > asProperties.maxInstanceCount)
        throw VK_ERROR_TOO_MANY_OBJECTS;
      std::vector<VkAccelerationStructureInstanceKHR> data;
      data.reserve(inputs[i]->size());
      {
        ScopedMilliseconds instances{trace::g_accelNow.instances};
        trace::g_accelNow.topInstances += uint32_t(inputs[i]->size());
        for (const auto &input : *inputs[i])
          data.push_back(instanceData(input, faceCulling));
        hostBuffer(top.instances, data.data(), data.size() * sizeof(data[0]), inputUsage);
      }
      geometry[i] = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                     .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR};
      geometry[i].geometry.instances = {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
          .data = {.deviceAddress = top.instances->address}};
      bool update = i == 1 && !reasons[i];
      builds[i] = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                   .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                   .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                            (i ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0u),
                   .mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                  : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                   .geometryCount = 1,
                   .pGeometries = &geometry[i]};
      sizes[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
      buildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &builds[i], &count[i], &sizes[i]);
      if (!top.accel || top.accel->size < sizes[i].accelerationStructureSize) {
        if (top.accel)
          retired.push_back(std::move(top.accel));
        top.accel =
            acceleration(sizes[i].accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
      }
      builds[i].srcAccelerationStructure = update ? top.accel->handle : VK_NULL_HANDLE;
      builds[i].dstAccelerationStructure = top.accel->handle;
      scratchBytes += align(update ? sizes[i].updateScratchSize : sizes[i].buildScratchSize,
                            asProperties.minAccelerationStructureScratchOffsetAlignment);
    }
    if (!changed[0] && !changed[1])
      return;
    if (!topScratch ||
        topScratch->size < scratchBytes + asProperties.minAccelerationStructureScratchOffsetAlignment)
      topScratch = buffer(scratchBytes + asProperties.minAccelerationStructureScratchOffsetAlignment,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDeviceAddress address =
        align(topScratch->address, asProperties.minAccelerationStructureScratchOffsetAlignment);
    for (unsigned i = 0; i < 2; ++i) {
      if (!changed[i])
        continue;
      bool update = builds[i].mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
      builds[i].scratchData.deviceAddress = address;
      VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = count[i]};
      const auto *ranges = &range;
      buildAs(command, 1, &builds[i], &ranges);
      address += align(update ? sizes[i].updateScratchSize : sizes[i].buildScratchSize,
                       asProperties.minAccelerationStructureScratchOffsetAlignment);
      tlas[i].policy.commit(*inputs[i], reasons[i]);
      if (!i) {
        tlasStaticGeneration = staticGeneration;
        ++stats.staticTlasBuilds;
      }
      else if (update)
        ++stats.dynamicTlasRefits;
      else
        ++stats.dynamicTlasBuilds;
    }
    stats.dynamicRefitCount = tlas[1].policy.refits;
  }

  // One TLAS over the static instances followed by the dynamic ones, so every
  // ray traverses once. Static changes rebuild it; dynamic transform changes
  // refit it (trace::CombinedTlasPolicy). The static instance records are kept
  // while the static inputs are unchanged, and each frame slot's instance
  // buffer keeps the static part it already holds.
  void PathTracer::Gpu::buildCombinedTop(VkCommandBuffer command,
                                         const std::vector<trace::InstanceInput> &staticInputs,
                                         const std::vector<trace::InstanceInput> &dynamicInputs, bool force,
                                         uint64_t staticGeneration) {
    using Policy = trace::CombinedTlasPolicy;
    auto &top = tlas[2];
    auto &costs = trace::g_accelNow;
    auto lapBegan = std::chrono::steady_clock::now();
    const auto lap = [&](double &into) {
      const auto now = std::chrono::steady_clock::now();
      into += std::chrono::duration<double, std::milli>(now - lapBegan).count();
      lapBegan = now;
    };
    const auto decision = combinedPolicy.decide(staticInputs, staticGeneration, dynamicInputs, force);
    lap(costs.decide);
    stats.dynamicRebuildReason = decision.action == Policy::Build ? decision.reason : 0;
    if (decision.action == Policy::None) {
      combinedPolicy.commit(staticInputs, staticGeneration, dynamicInputs, decision);
      stats.dynamicRefitCount = combinedPolicy.dynamicPart.refits;
      return;
    }
    const size_t total = staticInputs.size() + dynamicInputs.size();
    if (total > asProperties.maxInstanceCount)
      throw VK_ERROR_TOO_MANY_OBJECTS;
    const auto count = uint32_t(total);
    costs.topInstances += count;
    costs.staticChanged = decision.staticChanged;
    if (decision.staticChanged || !combinedStaticValid) {
      costs.staticFilled += uint32_t(staticInputs.size());
      combinedStatic.clear();
      combinedStatic.reserve(staticInputs.size());
      for (const auto &input : staticInputs)
        combinedStatic.push_back(instanceData(input, topFaceCulling, trace::instanceMask(true, false)));
      combinedStaticValid = true;
      ++combinedStaticStamp;
    }
    combinedDynamic.clear();
    combinedDynamic.reserve(dynamicInputs.size());
    for (const auto &input : dynamicInputs)
      combinedDynamic.push_back(instanceData(input, topFaceCulling, trace::instanceMask(true, true)));
    const size_t staticBytes = combinedStatic.size() * sizeof(combinedStatic[0]);
    const size_t bytes = staticBytes + combinedDynamic.size() * sizeof(combinedDynamic[0]);
    bool kept = top.prefix == combinedStaticStamp;
    lap(costs.fill);
    if (!top.instances || top.instances->size < bytes) {
      // (Room to spare and part of a block, as hostBuffer's.)
      if (top.instances)
        retiredBuffers.push_back(std::move(top.instances));
      top.instances = buffer(bytes + bytes / 2, inputUsage, true, true);
      kept = false;
    }
    lap(costs.storageInstances);
    write(*top.instances,
          {{combinedStatic.data(), staticBytes}, {combinedDynamic.data(), bytes - staticBytes}},
          kept ? staticBytes : 0);
    costs.written += kept ? bytes - staticBytes : bytes;
    costs.keptPrefix = kept;
    lap(costs.write);
    top.prefix = combinedStaticStamp;
    VkAccelerationStructureGeometryKHR geometry = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                                                   .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR};
    geometry.geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                                   .data = {.deviceAddress = top.instances->address}};
    bool update = decision.action == Policy::Update;
    VkAccelerationStructureBuildGeometryInfoKHR build = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                 VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .geometryCount = 1,
        .pGeometries = &geometry};
    VkAccelerationStructureBuildSizesInfoKHR sizes = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    buildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &count, &sizes);
    lap(costs.sizes);
    if (!top.accel || top.accel->size < sizes.accelerationStructureSize) {
      if (top.accel)
        retired.push_back(std::move(top.accel));
      top.accel = acceleration(sizes.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
      // A new structure has no source to refit.
      update = false;
      build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    }
    lap(costs.storageAccel);
    build.srcAccelerationStructure = update ? top.accel->handle : VK_NULL_HANDLE;
    build.dstAccelerationStructure = top.accel->handle;
    const VkDeviceSize alignment = asProperties.minAccelerationStructureScratchOffsetAlignment;
    const VkDeviceSize scratchBytes = align(update ? sizes.updateScratchSize : sizes.buildScratchSize, alignment);
    if (!topScratch || topScratch->size < scratchBytes + alignment)
      topScratch = buffer(scratchBytes + alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    build.scratchData.deviceAddress = align(topScratch->address, alignment);
    VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = count};
    const auto *ranges = &range;
    lap(costs.storage);
    buildAs(command, 1, &build, &ranges);
    costs.update = update;
    lap(costs.topRecord);
    // A refit forced into a build commits as one, restarting the refit count.
    auto committed = decision;
    if (!update && committed.action == Policy::Update) {
      committed.action = Policy::Build;
      committed.reason = REMIXAPI_REBUILD_MEMBERSHIP_EXT;
    }
    combinedPolicy.commit(staticInputs, staticGeneration, dynamicInputs, committed);
    if (decision.staticChanged)
      ++stats.staticTlasBuilds;
    if (update)
      ++stats.dynamicTlasRefits;
    else
      ++stats.dynamicTlasBuilds;
    stats.dynamicRefitCount = combinedPolicy.dynamicPart.refits;
  }

} // namespace remix
