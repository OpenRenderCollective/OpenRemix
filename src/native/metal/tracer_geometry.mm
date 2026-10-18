// Mesh views: pathtrace/geometry.cpp and the frame tables' view lookup
// (pathtrace/rendering.cpp TableViews), with Metal buffers and BLASes.
#include "metal/tracer_gpu.h"
#include <atomic>

namespace remix::metal {
  using namespace scene;
  using namespace metal::trace;

  namespace {
    uint64_t chainFingerprint(uint64_t contentHash, uint64_t seed = 1469598103934665603ull) {
      return scene::hashBytes(&contentHash, sizeof(contentHash), seed);
    }
  } // namespace

  std::shared_ptr<trace::Buffer> PathTracer::Gpu::buffer(const void *data, size_t size) {
    auto result = std::make_shared<Buffer>();
    result->handle = metal::buffer(device, data, size);
    result->address = result->handle.gpuAddress;
    result->size = size;
    addressable.push_back(result);
    return result;
  }

  void PathTracer::Gpu::collectResident() {
    resident.clear();
    std::erase_if(addressable, [&](const std::weak_ptr<Buffer> &entry) {
      const auto buffer = entry.lock();
      if (buffer)
        resident.push_back(buffer->handle);
      return !buffer;
    });
  }

  PathTracer::Gpu::MeshView &PathTracer::Gpu::meshView(const Draw &draw, const OpacityPartition *const *partitions,
                                                       size_t partitionCount, MeshConversion &conversion,
                                                       bool &convert) {
    ScopedTicks time{geometryTicks};
    convert = false;
    std::string opaque;
    for (size_t i = 0; i < partitionCount; ++i)
      opaque.append(partitions[i]->key);
    const bool skinned = draw.bones && std::any_of(draw.mesh->surfaces.begin(), draw.mesh->surfaces.end(),
                                                  [](const auto &s) { return s->bonesPerVertex != 0; });
    const bool streaming = bool(draw.mesh->updateStream);
    const bool deform = skinned || streaming;
    auto key =
        std::make_tuple(draw.mesh.get(), skinned ? draw.instanceId : 0, skinned ? draw.instanceIndex : 0, opaque);
    auto [entry, inserted] = meshes.try_emplace(key);
    auto &view = entry->second;
    if (inserted && std::get<1>(entry->first))
      deformViews.insert(&entry->first);
    if (streaming && view.owner.lock() != draw.mesh) {
      const UpdateViewKey lineage{draw.mesh->updateStream.get(), std::get<1>(key), std::get<2>(key), opaque};
      auto found = updateViews.find(lineage);
      if (found != updateViews.end() && found->second != key) {
        auto old = meshes.find(found->second);
        if (old != meshes.end() && old->second.updateStream == draw.mesh->updateStream &&
            old->second.lastUse != sequence) {
          if (view.geometry)
            frame().retiredGeometry.push_back(std::move(view.geometry));
          for (auto &vertices : view.vertices)
            frame().retiredBuffers.push_back(std::move(vertices));
          view = std::move(old->second);
          deformViews.erase(&old->first);
          meshes.erase(old);
        }
      }
      updateViews[lineage] = key;
    }
    view.lastUse = sequence;
    if (view.owner.lock() == draw.mesh && (!skinned || view.bones == *draw.bones))
      return view;
    ++performance.geometryConversions;
    const bool transfer = streaming && view.updateStream == draw.mesh->updateStream;
    conversion.previousGeometry = !transfer && view.owner.lock() != draw.mesh ? std::move(view.geometry) : nullptr;
    view.owner = draw.mesh;
    view.updateStream = draw.mesh->updateStream;
    if (skinned)
      view.bones = *draw.bones;
    conversion.view = &view;
    conversion.draw = &draw;
    conversion.partitions = partitions;
    conversion.opaque = std::move(opaque);
    conversion.skinned = skinned;
    conversion.streaming = streaming;
    conversion.deform = deform;
    convert = true;
    return view;
  }

  PathTracer::Gpu::MeshView *PathTracer::Gpu::findView(const Draw &draw, const OpacityPartition *const *partitions,
                                                       size_t partitionCount) {
    if (draw.mesh->updateStream)
      return nullptr;
    std::string opaque;
    for (size_t i = 0; i < partitionCount; ++i)
      opaque.append(partitions[i]->key);
    const bool skinned = draw.bones && std::any_of(draw.mesh->surfaces.begin(), draw.mesh->surfaces.end(),
                                                  [](const auto &s) { return s->bonesPerVertex != 0; });
    const MeshViewKey key{draw.mesh.get(), skinned ? draw.instanceId : 0, skinned ? draw.instanceIndex : 0,
                          std::move(opaque)};
    const auto found = meshes.find(key);
    if (found == meshes.end())
      return nullptr;
    auto &view = found->second;
    if (view.owner.owner_before(draw.mesh) || draw.mesh.owner_before(view.owner) || view.owner.expired() ||
        (skinned && view.bones != *draw.bones))
      return nullptr;
    std::atomic_ref<uint64_t>(view.lastUse).store(sequence, std::memory_order_relaxed);
    return &view;
  }

  void PathTracer::Gpu::convertMesh(MeshConversion &conversion) {
    const auto &draw = *conversion.draw;
    const auto *partitions = conversion.partitions;
    const auto &opaque = conversion.opaque;
    auto &data = conversion.data;
    auto &content = data.content;
    content.clear();
    size_t contentBytes = opaque.size() + sizeof(uint32_t);
    for (const auto &surface : draw.mesh->surfaces)
      contentBytes += 12 + surface->vertices.size() * 12 + surface->indices.size() * 4;
    content.reserve(contentBytes);
    auto append = [&](const void *p, size_t size) { content.append(static_cast<const char *>(p), size); };
    auto &shading = data.shading;
    auto &vertexOffsets = data.vertexOffsets, &indexOffsets = data.indexOffsets;
    auto &counts = data.counts, &vertexCounts = data.vertexCounts;
    auto &groups = data.groups;
    vertexOffsets.clear();
    indexOffsets.clear();
    counts.clear();
    vertexCounts.clear();
    groups.clear();
    const auto surfaceCount = draw.mesh->surfaces.size();
    const auto surfaceHeader = uint32_t(surfaceCount);
    append(&surfaceHeader, sizeof(surfaceHeader));
    shading.resize(surfaceCount);
    size_t surfaceIndex = 0;
    float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (const auto &surface : draw.mesh->surfaces) {
      uint32_t count[3] = {uint32_t(surface->vertices.size()), uint32_t(surface->indices.size()),
                           partitions[surfaceIndex]->key[0] == 2 ? 2u : 1u};
      append(count, sizeof(count));
      vertexOffsets.push_back(content.size());
      auto &vertices = shading[surfaceIndex];
      vertices.resize(surface->vertices.size());
      const size_t positions = content.size();
      content.resize(positions + surface->vertices.size() * 12);
      auto *position = content.data() + positions;
      for (size_t i = 0; i < surface->vertices.size(); ++i) {
        auto vertex = surface->vertices[i];
        if (surface->bonesPerVertex) {
          const auto original = vertex;
          std::fill_n(vertex.position, 3, 0.f);
          std::fill_n(vertex.normal, 3, 0.f);
          for (uint32_t b = 0; b < surface->bonesPerVertex; ++b) {
            size_t index = i * surface->bonesPerVertex + b;
            const auto &matrix = (*draw.bones)[surface->bones[index]];
            for (int r = 0; r < 3; ++r)
              vertex.position[r] += surface->weights[index] *
                                    (matrix[r] * original.position[0] + matrix[4 + r] * original.position[1] +
                                     matrix[8 + r] * original.position[2] + matrix[12 + r]);
            for (int r = 0; r < 3; ++r)
              vertex.normal[r] += surface->weights[index] * (matrix[r] * original.normal[0] +
                                                             matrix[4 + r] * original.normal[1] +
                                                             matrix[8 + r] * original.normal[2]);
          }
          const float length = std::hypot(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
          if (std::isfinite(length) && length > 1e-12f)
            for (float &component : vertex.normal)
              component /= length;
          else
            std::fill_n(vertex.normal, 3, 0.f);
        }
        std::memcpy(position + i * 12, vertex.position, 12);
        for (int r = 0; r < 3; ++r) {
          low[r] = std::min(low[r], vertex.position[r]);
          high[r] = std::max(high[r], vertex.position[r]);
        }
        auto &out = vertices[i];
        std::copy_n(vertex.position, 3, out.position);
        std::copy_n(vertex.uv, 2, out.uv);
        std::copy_n(vertex.color, 4, out.color);
        out.position[3] = vertex.normal[0];
        out.uv[2] = vertex.normal[1];
        out.uv[3] = vertex.normal[2];
      }
      indexOffsets.push_back(content.size());
      append(surface->indices.data(), surface->indices.size() * 4);
      const auto &partition = *partitions[surfaceIndex];
      if (partition.key[0] != 2) {
        groups.push_back({uint32_t(surfaceIndex), count[1] / 3, indexOffsets.back(), 0, partition.key[0] == 1});
        counts.push_back(count[1] / 3);
      } else {
        for (bool solid : {true, false}) {
          GeometryGroup group{uint32_t(surfaceIndex), 0, content.size(), 0, solid};
          for (uint32_t triangle = 0; triangle < partition.triangles; ++triangle)
            if (partition.opaque(triangle) == solid) {
              append(surface->indices.data() + triangle * 3, 12);
              ++group.count;
            }
          group.triangleMap = content.size();
          for (uint32_t triangle = 0; triangle < partition.triangles; ++triangle)
            if (partition.opaque(triangle) == solid)
              append(&triangle, sizeof(triangle));
          counts.push_back(group.count);
          groups.push_back(group);
        }
      }
      vertexCounts.push_back(count[0]);
      ++surfaceIndex;
    }
    content.append(opaque);
    std::copy_n(low, 3, data.low);
    std::copy_n(high, 3, data.high);
    data.contentHash = scene::fastHash(content.data(), content.size());
    data.shadingHashes.clear();
    for (const auto &vertices : shading)
      data.shadingHashes.push_back(scene::fastHash(vertices.data(), vertices.size() * sizeof(TraceVertex)));
  }

  void PathTracer::Gpu::commitMesh(MeshConversion &conversion) {
    ScopedTicks time{geometryTicks};
    auto &view = *conversion.view;
    const auto &data = conversion.data;
    const auto &content = data.content;
    const bool deform = conversion.deform;
    if (!deform) {
      auto found = geometryCache.find(content);
      if (found != geometryCache.end())
        view.geometry = found->second.lock();
      if (view.geometry)
        ++performance.geometryCacheHits;
    }
    if (!view.geometry || deform) {
      // Deforming geometry is rebuilt (the Vulkan tracer may refit it).
      if (view.geometry)
        frame().retiredGeometry.push_back(std::move(view.geometry));
      view.geometry = std::make_shared<Geometry>();
      auto &g = *view.geometry;
      g.deform = deform;
      g.pending = true;
      g.counts = data.counts;
      g.vertexCounts = data.vertexCounts;
      g.vertexOffsets = data.vertexOffsets;
      g.indexOffsets = data.indexOffsets;
      g.groups = data.groups;
      g.fingerprint = chainFingerprint(data.contentHash);
      ++g.revision;
      g.diagonal = std::hypot(data.high[0] - data.low[0], data.high[1] - data.low[1], data.high[2] - data.low[2]);
      g.input = buffer(content.data(), content.size());
      g.accel = std::make_shared<Accel>();
      g.accel->address = nextAccel++;
      accels[g.accel->address] = g.accel;
      pendingBuilds.push_back(view.geometry);
      if (!deform)
        geometryCache[content] = view.geometry;
    }
    for (auto &vertices : view.vertices)
      frame().retiredBuffers.push_back(std::move(vertices));
    view.vertices.clear();
    view.fingerprint = view.geometry->fingerprint;
    for (size_t i = 0; i < data.shading.size(); ++i) {
      const auto &vertices = data.shading[i];
      view.fingerprint = chainFingerprint(data.shadingHashes[i], view.fingerprint);
      view.vertices.push_back(buffer(vertices.data(), vertices.size() * sizeof(TraceVertex)));
    }
    if (conversion.previousGeometry)
      frame().retiredGeometry.push_back(std::move(conversion.previousGeometry));
    if (conversion.data.content.capacity() > 64 * 1024 * 1024)
      conversion.data = {};
  }

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
    g.partitionStorage.assign(partitionTotal, nullptr);
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
        g.commitMesh(g.conversions[g.instanceViews[index].conversion]);
      views[index] = g.instanceViews[index].view;
    }
  }

  bool PathTracer::Gpu::TableViews::findViews(const std::vector<Span> &instances,
                                              std::vector<const MeshView *> &views) {
    auto &g = *gpu;
    views.resize(instances.size());
    std::vector<const OpacityPartition *> partitions;
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

  // BLAS builds of the views converted this frame (pathtrace/blas.cpp,
  // without refits or compaction).
  void PathTracer::Gpu::buildBottom(id<MTLCommandBuffer> command) {
    std::erase_if(pendingBuilds, [](const auto &geometry) { return geometry.use_count() == 1; });
    if (pendingBuilds.empty())
      return;
    id<MTLAccelerationStructureCommandEncoder> encoder = accelerationEncoder(command, @"BLAS builds");
    for (const auto &geometry : pendingBuilds) {
      auto &g = *geometry;
      NSMutableArray<MTLAccelerationStructureTriangleGeometryDescriptor *> *descriptors = [NSMutableArray new];
      for (const auto &group : g.groups) {
        auto *triangles = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
        triangles.vertexBuffer = g.input->handle;
        triangles.vertexBufferOffset = g.vertexOffsets[group.surface];
        triangles.vertexStride = 12;
        triangles.vertexFormat = MTLAttributeFormatFloat3;
        triangles.indexBuffer = g.input->handle;
        triangles.indexBufferOffset = group.indices;
        triangles.indexType = MTLIndexTypeUInt32;
        triangles.triangleCount = group.count;
        triangles.opaque = group.opaque;
        // Any-hit candidates are reported to the query, as without
        // VK_GEOMETRY_OPAQUE_BIT_KHR; duplicate any-hit calls are allowed.
        triangles.allowDuplicateIntersectionFunctionInvocation = YES;
        [descriptors addObject:triangles];
      }
      auto *descriptor = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
      descriptor.geometryDescriptors = descriptors;
      descriptor.usage = MTLAccelerationStructureUsageNone;
      g.accel->handle = build(encoder, descriptor);
      g.pending = false;
    }
    [encoder endEncoding];
    pendingBuilds.clear();
  }

  id<MTLAccelerationStructure> PathTracer::Gpu::build(id<MTLAccelerationStructureCommandEncoder> encoder,
                                                      MTLAccelerationStructureDescriptor *descriptor) {
    const auto sizes = [device accelerationStructureSizesWithDescriptor:descriptor];
    id<MTLAccelerationStructure> structure =
        [device newAccelerationStructureWithSize:std::max<NSUInteger>(sizes.accelerationStructureSize, 256)];
    check(structure, "acceleration structure");
    // Builds in one encoder may run concurrently: each gets its own scratch.
    id<MTLBuffer> scratchBuffer = [device newBufferWithLength:std::max<NSUInteger>(sizes.buildScratchBufferSize, 256)
                                                      options:MTLResourceStorageModePrivate];
    check(scratchBuffer, "acceleration structure scratch");
    retire(scratchBuffer);
    [encoder buildAccelerationStructure:structure descriptor:descriptor scratchBuffer:scratchBuffer scratchBufferOffset:0];
    stats.accelerationBytes += sizes.accelerationStructureSize;
    return structure;
  }
} // namespace remix::metal
