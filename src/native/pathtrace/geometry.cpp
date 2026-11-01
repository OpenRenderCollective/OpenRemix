#include "pathtrace/gpu.h"
#include "runtime/pipelines.h"
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  namespace {
    // REMIX_GPU_SKINNING=0 skins every frame on the CPU (comparison and diagnosis).
    bool gpuSkinning() {
      static const bool enabled = [] {
        const char *value = std::getenv("REMIX_GPU_SKINNING");
        return !value || std::strcmp(value, "0") != 0;
      }();
      return enabled;
    }
    // These fingerprints only invalidate history inside this renderer run.
    // Bulk hashing avoids a serial byte-at-a-time multiply over every changed
    // vertex (std::hash<string_view> is byte-wise FNV-1a in MSVC's library).
    // Geometry cache reuse still compares the complete content string.
    uint64_t chainFingerprint(uint64_t contentHash, uint64_t seed = 1469598103934665603ull) {
      return scene::hashBytes(&contentHash, sizeof(contentHash), seed);
    }
  } // namespace

  VkBuildAccelerationStructureFlagsKHR PathTracer::Gpu::flags(bool deform) const {
    return VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
           (deform ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                   : VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);
  }

  PathTracer::Gpu::MeshView &PathTracer::Gpu::mesh(VkCommandBuffer command, const Draw &draw,
      const std::vector<const scene::OpacityPartition *> &partitions) {
    MeshConversion conversion;
    bool convert = false;
    auto &view = meshView(draw, partitions.data(), partitions.size(), conversion, convert);
    if (convert) {
      {
        ScopedTicks time{geometryTicks};
        convertMesh(conversion);
      }
      commitMesh(command, conversion);
    }
    return view;
  }

  PathTracer::Gpu::MeshView &PathTracer::Gpu::meshView(const Draw &draw,
      const scene::OpacityPartition *const *partitions, size_t partitionCount, MeshConversion &conversion,
      bool &convert) {
    ScopedTicks time{geometryTicks};
    convert = false;
    std::string opaque;
    for (size_t i = 0; i < partitionCount; ++i) opaque.append(partitions[i]->key);
    const bool skinned = draw.bones && std::any_of(draw.mesh->surfaces.begin(), draw.mesh->surfaces.end(),
                                                  [](const auto &s) { return s->bonesPerVertex != 0; });
    const bool streaming = bool(draw.mesh->updateStream);
    const bool deform = skinned || streaming;
    // Each independently animated skin owns its BLAS. Opt-in CPU replacements
    // may transfer compatible storage only from a view unused in this frame;
    // a skinned lineage transfers per instance.
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
          if (view.geometry) retiredGeometry.push_back(std::move(view.geometry));
          for (auto &vertices : view.vertices) retiredBuffers.push_back(std::move(vertices));
          view = std::move(old->second); deformViews.erase(&old->first); meshes.erase(old);
        }
      }
      updateViews[lineage] = key;
    }
    view.lastUse = sequence;
    if (view.owner.lock() == draw.mesh && (!skinned || view.bones == *draw.bones))
      return view;
    ++performance.geometryConversions;
    // The same skinned mesh with other bones, its bind pose on the GPU: skinned
    // there (commitSkin), with no CPU geometry.
    conversion.gpuSkin = skinned && !streaming && skin && view.geometry && view.owner.lock() == draw.mesh &&
                         view.skinMesh.lock() == draw.mesh &&
                         view.skinSource.size() == draw.mesh->surfaces.size() && view.skinInputBytes;
    // A replaced CPU mesh may reuse an address. Keep its geometry alive through
    // the canonical lookup, but never mistake its view for the new mesh.
    const bool transfer = streaming && view.updateStream == draw.mesh->updateStream;
    conversion.previousGeometry = !transfer && view.owner.lock() != draw.mesh ? std::move(view.geometry) : nullptr;
    // Later draws of this view this frame see it as converted, as they did when
    // conversion completed before the next lookup.
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

  PathTracer::Gpu::MeshView *PathTracer::Gpu::findView(const Draw &draw,
      const scene::OpacityPartition *const *partitions, size_t partitionCount) {
    if (draw.mesh->updateStream)
      return nullptr;
    std::string opaque;
    for (size_t i = 0; i < partitionCount; ++i) opaque.append(partitions[i]->key);
    const bool skinned = draw.bones && std::any_of(draw.mesh->surfaces.begin(), draw.mesh->surfaces.end(),
                                                  [](const auto &s) { return s->bonesPerVertex != 0; });
    const MeshViewKey key{draw.mesh.get(), skinned ? draw.instanceId : 0, skinned ? draw.instanceIndex : 0,
                          std::move(opaque)};
    const auto found = meshes.find(key);
    if (found == meshes.end())
      return nullptr;
    auto &view = found->second;
    // view.owner.lock() == draw.mesh without the reference-count traffic.
    if (view.owner.owner_before(draw.mesh) || draw.mesh.owner_before(view.owner) || view.owner.expired() ||
        (skinned && view.bones != *draw.bones))
      return nullptr;
    std::atomic_ref<uint64_t>(view.lastUse).store(sequence, std::memory_order_relaxed);
    return &view;
  }

  void PathTracer::Gpu::convertMesh(MeshConversion &conversion) {
    if (conversion.gpuSkin)
      return;
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
    vertexOffsets.clear(); indexOffsets.clear(); counts.clear(); vertexCounts.clear(); groups.clear();
    const auto surfaceCount = draw.mesh->surfaces.size();
    const auto surfaceHeader = uint32_t(surfaceCount);
    append(&surfaceHeader, sizeof(surfaceHeader));
    shading.resize(surfaceCount);
    size_t surfaceIndex = 0;
    vertexOffsets.reserve(surfaceCount); indexOffsets.reserve(surfaceCount);
    counts.reserve(surfaceCount); vertexCounts.reserve(surfaceCount);
    float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
    // Every byte is hashed once: per-surface positions, attributes (texture
    // coordinates and colors) and normals here, and the remaining content
    // (counts, indices, partitions, opacity) as the topology below. The
    // content and shading hashes combine those parts.
    data.positionHashes.clear(); data.attributeHashes.clear(); data.normalHashes.clear();
    auto &attributes = data.attributeScratch, &normals = data.normalScratch;
    for (const auto &surface : draw.mesh->surfaces) {
      uint32_t count[3] = {uint32_t(surface->vertices.size()), uint32_t(surface->indices.size()),
                           partitions[surfaceIndex]->key[0] == 2 ? 2u : 1u};
      append(count, sizeof(count));
      vertexOffsets.push_back(content.size());
      auto &vertices = shading[surfaceIndex];
      vertices.resize(surface->vertices.size());
      // Positions go straight into the reserved content buffer.
      const size_t positions = content.size();
      content.resize(positions + surface->vertices.size() * 12);
      auto *position = content.data() + positions;
      attributes.resize(surface->vertices.size() * 6);
      normals.resize(surface->vertices.size() * 3);
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
              vertex.normal[r] += surface->weights[index] *
                  (matrix[r] * original.normal[0] + matrix[4 + r] * original.normal[1] +
                   matrix[8 + r] * original.normal[2]);
          }
          // Unit-scale normals: no overflow guard needed (std::hypot is far slower).
          const float length = std::sqrt(vertex.normal[0] * vertex.normal[0] + vertex.normal[1] * vertex.normal[1] +
                                         vertex.normal[2] * vertex.normal[2]);
          if (std::isfinite(length) && length > 1e-12f)
            for (float &component : vertex.normal) component /= length;
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
        // Reuse the existing shading-vertex padding, preserving its 48-byte
        // stride and the position-only BLAS input size.
        out.position[3] = vertex.normal[0];
        out.uv[2] = vertex.normal[1]; out.uv[3] = vertex.normal[2];
        std::copy_n(vertex.uv, 2, attributes.data() + i * 6);
        std::copy_n(vertex.color, 4, attributes.data() + i * 6 + 2);
        std::copy_n(vertex.normal, 3, normals.data() + i * 3);
      }
      data.positionHashes.push_back(scene::fastHash(position, surface->vertices.size() * 12));
      data.attributeHashes.push_back(scene::fastHash(attributes.data(), attributes.size() * sizeof(float)));
      data.normalHashes.push_back(scene::fastHash(normals.data(), normals.size() * sizeof(float)));
      indexOffsets.push_back(content.size());
      append(surface->indices.data(), surface->indices.size() * 4);
      const auto &partition = *partitions[surfaceIndex];
      if (partition.key[0] != 2) {
        groups.push_back({uint32_t(surfaceIndex), count[1] / 3, indexOffsets.back(), 0, partition.key[0] == 1});
        counts.push_back(count[1] / 3);
      } else {
        // AS/shading indices share the partition order. Raster indices, vertex
        // correspondence and emitter/debug identities retain their original order.
        for (bool solid : {true, false}) {
          GeometryGroup group{uint32_t(surfaceIndex), 0, content.size(), 0, solid};
          for (uint32_t triangle = 0; triangle < partition.triangles; ++triangle)
            if (partition.opaque(triangle) == solid) {
              append(surface->indices.data() + triangle * 3, 12); ++group.count;
            }
          group.triangleMap = content.size();
          for (uint32_t triangle = 0; triangle < partition.triangles; ++triangle)
            if (partition.opaque(triangle) == solid) append(&triangle, sizeof(triangle));
          counts.push_back(group.count); groups.push_back(group);
        }
      }
      vertexCounts.push_back(count[0]);
      ++surfaceIndex;
    }
    // Opacity is part of BLAS identity, including independently skinned and
    // streamed variants. Append after geometry data to preserve input alignment.
    content.append(opaque);
    std::copy_n(low, 3, data.low);
    std::copy_n(high, 3, data.high);
    // Delta-upload keys: every content byte except positions (counts, indices,
    // partition order, opacity), and each surface's attributes and normals.
    uint64_t topology = 0;
    size_t at = 0;
    for (size_t i = 0; i < vertexOffsets.size(); ++i) {
      const size_t begin = size_t(vertexOffsets[i]), end = begin + size_t(vertexCounts[i]) * 12;
      topology = chainFingerprint(scene::fastHash(content.data() + at, begin - at), topology);
      at = end;
    }
    data.topologyHash = chainFingerprint(scene::fastHash(content.data() + at, content.size() - at), topology);
    data.contentHash = data.topologyHash;
    data.shadingHashes.clear();
    for (size_t i = 0; i < shading.size(); ++i) {
      data.contentHash = chainFingerprint(data.positionHashes[i], data.contentHash);
      // TraceVertex holds exactly position, normal, texture coordinates and color.
      data.shadingHashes.push_back(chainFingerprint(
          data.normalHashes[i], chainFingerprint(data.attributeHashes[i], data.positionHashes[i])));
    }
  }

  void PathTracer::Gpu::createAssemblePipeline() {
    if (!assembleLayout) {
      const VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AssembleJob)};
      const VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .pushConstantRangeCount = 1,
                                               .pPushConstantRanges = &range};
      if (pipeline::createPipelineLayout(device, &info, nullptr, &assembleLayout) != VK_SUCCESS) {
        assembleFailed = true;
        return;
      }
    }
    const auto code = trace::shader(trace::Stage::vertexAssemble);
    const VkShaderModuleCreateInfo moduleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code.bytes, .pCode = code.code};
    VkShaderModule module{};
    if (pipeline::createShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
      assembleFailed = true;
      return;
    }
    const VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = assembleLayout};
    const auto result = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &assemble);
    pipeline::destroyShaderModule(device, module, nullptr);
    // Off the compiler worker a new pipeline is not ready yet: full uploads until it is.
    if (result != VK_SUCCESS) {
      assemble = VK_NULL_HANDLE;
      assembleFailed = result != VK_NOT_READY;
    }
  }

  void PathTracer::Gpu::recordAssemble(VkCommandBuffer command) {
    if (assembleJobs.empty() && skinJobs.empty())
      return;
    // Positions (and the inputs skinning writes over) were copied this frame.
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (!assembleJobs.empty()) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, assemble);
      for (const auto &job : assembleJobs) {
        vkCmdPushConstants(command, assembleLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(job), &job);
        vkCmdDispatch(command, (job.count + 63) / 64, 1, 1);
      }
    }
    if (!skinJobs.empty()) {
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, skin);
      for (const auto &job : skinJobs) {
        vkCmdPushConstants(command, skinLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(job), &job);
        vkCmdDispatch(command, (job.count + 63) / 64, 1, 1);
      }
    }
    assembleJobs.clear();
    skinJobs.clear();
  }

  void PathTracer::Gpu::createSkinPipeline() {
    if (!skinLayout) {
      const VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkinJob)};
      const VkPipelineLayoutCreateInfo info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .pushConstantRangeCount = 1,
                                               .pPushConstantRanges = &range};
      if (pipeline::createPipelineLayout(device, &info, nullptr, &skinLayout) != VK_SUCCESS) {
        skinFailed = true;
        return;
      }
    }
    const auto code = trace::shader(trace::Stage::vertexSkin);
    const VkShaderModuleCreateInfo moduleInfo = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = code.bytes, .pCode = code.code};
    VkShaderModule module{};
    if (pipeline::createShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
      skinFailed = true;
      return;
    }
    const VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main"},
        .layout = skinLayout};
    const auto result = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &skin);
    pipeline::destroyShaderModule(device, module, nullptr);
    // Off the compiler worker a new pipeline is not ready yet: CPU skinning until it is.
    if (result != VK_SUCCESS) {
      skin = VK_NULL_HANDLE;
      skinFailed = result != VK_NOT_READY;
    }
  }

  void PathTracer::Gpu::uploadSkinSource(VkCommandBuffer command, MeshConversion &conversion) {
    if (!gpuSkinning() || skinFailed)
      return;
    if (!skin)
      createSkinPipeline();
    auto &view = *conversion.view;
    const auto &draw = *conversion.draw;
    const auto &data = conversion.data;
    view.skinOffsets = data.vertexOffsets;
    view.skinCounts = data.vertexCounts;
    view.skinInputBytes = data.content.size();
    if (view.skinMesh.lock() == draw.mesh && view.skinSource.size() == draw.mesh->surfaces.size())
      return;
    view.skinSource.clear();
    view.skinWeights.clear();
    view.skinIndices.clear();
    view.skinBonesPerVertex.clear();
    std::vector<TraceVertex> bind;
    for (const auto &surface : draw.mesh->surfaces) {
      // The bind pose as convertMesh lays out an unskinned vertex.
      bind.resize(surface->vertices.size());
      for (size_t i = 0; i < bind.size(); ++i) {
        const auto &vertex = surface->vertices[i];
        auto &out = bind[i];
        std::copy_n(vertex.position, 3, out.position);
        out.position[3] = vertex.normal[0];
        std::copy_n(vertex.uv, 2, out.uv);
        out.uv[2] = vertex.normal[1];
        out.uv[3] = vertex.normal[2];
        std::copy_n(vertex.color, 4, out.color);
      }
      view.skinSource.push_back(
          upload(command, bind.data(), bind.size() * sizeof(TraceVertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
      if (surface->bonesPerVertex) {
        view.skinWeights.push_back(upload(command, surface->weights.data(), surface->weights.size() * sizeof(float),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        view.skinIndices.push_back(upload(command, surface->bones.data(), surface->bones.size() * sizeof(uint32_t),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
      } else {
        view.skinWeights.emplace_back();
        view.skinIndices.emplace_back();
      }
      view.skinBonesPerVertex.push_back(surface->bonesPerVertex);
    }
    view.skinMesh = draw.mesh;
  }

  void PathTracer::Gpu::commitSkin(VkCommandBuffer command, MeshConversion &conversion) {
    ScopedTicks time{geometryTicks};
    auto &view = *conversion.view;
    auto &g = *view.geometry;
    const auto &bones = *conversion.draw->bones;
    const size_t boneBytes = bones.size() * sizeof(scene::Matrix);
    // Read in place from the staging buffer (a few kilobytes per instance).
    const VkDeviceSize boneOffset = stage(bones.data(), boneBytes);
    const VkDeviceAddress bonesAt = staging.buffer->address + boneOffset;
    const uint64_t bonesHash = scene::fastHash(bones.data(), boneBytes);
    // A new BLAS input (the previous one may still be read in flight): every
    // byte copied, the positions then written over by the skinning pass.
    constexpr VkBufferUsageFlags meshInput =
        inputUsage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    std::shared_ptr<Buffer> previousInput = std::move(g.input);
    g.input = buffer(view.skinInputBytes, meshInput | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true);
    const VkBufferCopy all = {.size = view.skinInputBytes};
    vkCmdCopyBuffer(command, previousInput->handle, g.input->handle, 1, &all);
    retiredBuffers.push_back(std::move(previousInput));
    for (size_t j = 0; j < g.geometries.size() && j < g.groups.size(); ++j) {
      auto &triangles = g.geometries[j].geometry.triangles;
      triangles.vertexData.deviceAddress = g.input->address + view.skinOffsets[g.groups[j].surface];
      triangles.indexData.deviceAddress = g.input->address + g.groups[j].indices;
    }
    // Skins keep refitting (their bones move the whole bound), as on the CPU path.
    g.update = true;
    ++g.refits;
    g.pending = true;
    ++g.revision;
    g.fingerprint = chainFingerprint(bonesHash, g.topologyHash);
    // New shading buffers: the previous ones feed motion vectors.
    for (auto &vertices : view.vertices)
      retiredBuffers.push_back(std::move(vertices));
    view.vertices.clear();
    view.fingerprint = g.fingerprint;
    for (size_t i = 0; i < view.skinSource.size(); ++i) {
      const uint32_t count = view.skinCounts[i];
      std::shared_ptr<Buffer> target =
          buffer(VkDeviceSize(count) * sizeof(TraceVertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, true);
      if (count)
        skinJobs.push_back({.source = view.skinSource[i]->address,
                            .weights = view.skinWeights[i] ? view.skinWeights[i]->address : 0,
                            .indices = view.skinIndices[i] ? view.skinIndices[i]->address : 0,
                            .bones = bonesAt,
                            .positions = g.input->address + view.skinOffsets[i],
                            .target = target->address,
                            .count = count,
                            .bonesPerVertex = view.skinBonesPerVertex[i]});
      view.fingerprint = chainFingerprint(chainFingerprint(bonesHash, i), view.fingerprint);
      view.vertices.push_back(std::move(target));
    }
  }

  void PathTracer::Gpu::commitMesh(VkCommandBuffer command, MeshConversion &conversion) {
    if (conversion.gpuSkin) {
      commitSkin(command, conversion);
      return;
    }
    ScopedTicks time{geometryTicks};
    auto &view = *conversion.view;
    const auto &data = conversion.data;
    const auto &content = data.content;
    const auto &low = data.low, &high = data.high;
    const bool deform = conversion.deform, skinned = conversion.skinned, streaming = conversion.streaming;
    // Delta upload: an update of a deforming view that keeps every content byte
    // except positions, and each surface's texture coordinates and colors, sends
    // only positions and normals. Its buffers are still new (previous versions
    // feed motion vectors); the rest is copied on the GPU.
    constexpr uint32_t assembleLimit = 65535u * 64u; // One dispatch per surface.
    // REMIX_DELTA_UPLOADS=0 always uploads in full (comparison and diagnosis).
    static const bool deltaUploads = [] {
      const char *value = std::getenv("REMIX_DELTA_UPLOADS");
      return !value || std::strcmp(value, "0") != 0;
    }();
    if (deform && !assemble && !assembleFailed)
      createAssemblePipeline();
    bool delta = false;
    if (!deform) {
      auto found = geometryCache.find(content);
      if (found != geometryCache.end())
        view.geometry = found->second.lock();
      if (view.geometry) ++performance.geometryCacheHits;
    }
    if (!view.geometry || deform) {
      bool update = view.geometry != nullptr;
      if (!view.geometry)
        view.geometry = std::make_shared<Geometry>();
      auto &g = *view.geometry;
      g.deform = deform;
      // Refit only while the build remains spatially similar, and periodically
      // rebuild to bound degradation from many small deformations. Skins keep
      // refitting as without a lineage: their bones move the whole bound.
      if (update && streaming && !skinned) {
        const float limit = std::max(g.diagonal * .25f, .001f);
        if (g.refits >= 60) update = false;
        for (unsigned c = 0; c < 3; ++c)
          if (std::abs(low[c] - g.buildLow[c]) > limit || std::abs(high[c] - g.buildHigh[c]) > limit) update = false;
      }
      g.update = update;
      if (update) ++g.refits;
      else {
        g.refits = 0;
        std::copy_n(low, 3, g.buildLow.begin()); std::copy_n(high, 3, g.buildHigh.begin());
      }
      g.pending = true;
      g.counts = data.counts;
      g.indexOffsets = data.indexOffsets;
      g.groups = data.groups;
      g.fingerprint = chainFingerprint(data.contentHash);
      ++g.revision;
      g.diagonal = std::hypot(high[0] - low[0], high[1] - low[1], high[2] - low[2]);
      constexpr VkBufferUsageFlags meshInput =
          inputUsage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      delta = deltaUploads && assemble && deform && g.input && g.topologyHash == data.topologyHash &&
              view.attributeHashes == data.attributeHashes && view.vertices.size() == data.shading.size() &&
              std::all_of(data.vertexCounts.begin(), data.vertexCounts.end(),
                          [](uint32_t count) { return count <= assembleLimit; });
      std::shared_ptr<Buffer> previousInput = std::move(g.input);
      if (delta) {
        g.input = buffer(content.size(), meshInput | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true);
        std::vector<VkBufferCopy> kept;
        VkDeviceSize at = 0;
        for (size_t i = 0; i < data.vertexOffsets.size(); ++i) {
          const VkDeviceSize begin = data.vertexOffsets[i];
          if (begin > at)
            kept.push_back({at, at, begin - at});
          at = begin + VkDeviceSize(data.vertexCounts[i]) * 12;
        }
        if (content.size() > at)
          kept.push_back({at, at, content.size() - at});
        if (!kept.empty())
          vkCmdCopyBuffer(command, previousInput->handle, g.input->handle, uint32_t(kept.size()), kept.data());
        for (size_t i = 0; i < data.vertexOffsets.size(); ++i) {
          const VkDeviceSize bytes = VkDeviceSize(data.vertexCounts[i]) * 12;
          if (!bytes)
            continue;
          const VkDeviceSize offset = stage(content.data() + data.vertexOffsets[i], size_t(bytes));
          const VkBufferCopy positions = {.srcOffset = offset, .dstOffset = data.vertexOffsets[i], .size = bytes};
          vkCmdCopyBuffer(command, staging.buffer->handle, g.input->handle, 1, &positions);
        }
      } else
        g.input = upload(command, content.data(), content.size(), meshInput);
      if (previousInput)
        retiredBuffers.push_back(std::move(previousInput));
      g.topologyHash = data.topologyHash;
      g.geometries.clear();
      g.ranges.clear();
      if (data.groups.size() > asProperties.maxGeometryCount) throw VK_ERROR_TOO_MANY_OBJECTS;
      for (const auto &group : data.groups) {
        const auto i = group.surface;
        VkAccelerationStructureGeometryKHR geometry = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .flags = group.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : VkGeometryFlagsKHR(0)};
        geometry.geometry.triangles = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData = {.deviceAddress = g.input->address + data.vertexOffsets[i]},
            .vertexStride = 12,
            .maxVertex = data.vertexCounts[i] - 1,
            .indexType = VK_INDEX_TYPE_UINT32,
            .indexData = {.deviceAddress = g.input->address + group.indices}};
        g.geometries.push_back(geometry);
        g.ranges.push_back({.primitiveCount = group.count});
      }
      VkAccelerationStructureBuildGeometryInfoKHR build = {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
          .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
          .flags = flags(deform),
          .geometryCount = uint32_t(g.geometries.size()),
          .pGeometries = g.geometries.data()};
      if (!g.accel) buildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, data.counts.data(), &g.sizes);
      if (!g.accel)
        g.accel =
            acceleration(g.sizes.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
      if (!deform)
        geometryCache[content] = view.geometry;
    }
    // Previous shading buffers stay alive until this slot's fence: delta jobs read them.
    auto previousVertices = std::move(view.vertices);
    view.vertices.clear();
    view.fingerprint = view.geometry->fingerprint;
    if (digestEnabled) {
      view.contentDigest = scene::hashBytes(content.data(), content.size());
      for (const auto &vertices : data.shading)
        view.contentDigest =
            scene::hashBytes(vertices.data(), vertices.size() * sizeof(TraceVertex), view.contentDigest);
    }
    std::vector<float> normals;
    for (size_t i = 0; i < data.shading.size(); ++i) {
      const auto &vertices = data.shading[i];
      view.fingerprint = chainFingerprint(data.shadingHashes[i], view.fingerprint);
      if (!delta || vertices.empty()) {
        view.vertices.push_back(upload(command, vertices.data(), vertices.size() * sizeof(TraceVertex),
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        continue;
      }
      auto target = buffer(vertices.size() * sizeof(TraceVertex), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, true);
      // Deformations that keep their normals (e.g. CPU wind displacement) send
      // positions only; assembly keeps the previous version's normals.
      if (i < view.normalHashes.size() && view.normalHashes[i] == data.normalHashes[i]) {
        assembleJobs.push_back({.positions = view.geometry->input->address + data.vertexOffsets[i],
                                .previous = previousVertices[i]->address,
                                .target = target->address,
                                .count = uint32_t(vertices.size())});
        view.vertices.push_back(std::move(target));
        continue;
      }
      // Normals live in the shading vertex padding (convertMesh).
      normals.resize(vertices.size() * 3);
      for (size_t v = 0; v < vertices.size(); ++v) {
        normals[v * 3] = vertices[v].position[3];
        normals[v * 3 + 1] = vertices[v].uv[2];
        normals[v * 3 + 2] = vertices[v].uv[3];
      }
      // Copied into device memory like the positions: the assembly shader then
      // reads them locally instead of across the host link.
      const VkDeviceSize bytes = normals.size() * sizeof(float);
      const VkDeviceSize offset = stage(normals.data(), size_t(bytes));
      std::shared_ptr<Buffer> uploaded =
          buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true);
      const VkBufferCopy copy = {.srcOffset = offset, .size = bytes};
      vkCmdCopyBuffer(command, staging.buffer->handle, uploaded->handle, 1, &copy);
      assembleJobs.push_back({.positions = view.geometry->input->address + data.vertexOffsets[i],
                              .normals = uploaded->address,
                              .previous = previousVertices[i]->address,
                              .target = target->address,
                              .count = uint32_t(vertices.size())});
      view.vertices.push_back(std::move(target));
      retiredBuffers.push_back(std::move(uploaded)); // Read by this frame's assembly.
    }
    for (auto &vertices : previousVertices)
      retiredBuffers.push_back(std::move(vertices));
    view.attributeHashes = data.attributeHashes;
    view.normalHashes = data.normalHashes;
    if (conversion.previousGeometry)
      retiredGeometry.push_back(std::move(conversion.previousGeometry));
    // Its bind pose, so the next frames skin on the GPU.
    if (skinned)
      uploadSkinSource(command, conversion);
    // Bound the scratch a single large conversion leaves behind.
    if (conversion.data.bytes() > 64 * 1024 * 1024)
      conversion.data = {};
  }

} // namespace remix
