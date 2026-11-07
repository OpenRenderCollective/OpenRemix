#include "usd/submission.h"
#include "usd/alpha_state.h"
#include "usd/mods.h"
#include "usd/render_categories.h"
#include "platform/utf8_path.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
namespace remix::usd {
  namespace {
    // The replacement a prim belongs to: /RootNode/meshes/mesh_<hash> or
    // /RootNode/lights/light_<hash>.
    std::string replacementRoot(const std::string &id) {
      for (const std::string scope : {"/RootNode/meshes/", "/RootNode/lights/"})
        if (id.starts_with(scope))
          return id.substr(0, id.find('/', scope.size()));
      return std::string();
    }
    uint64_t hash64(const std::string &value) {
      uint64_t h = 14695981039346656037ull;
      for (unsigned char c : value) {
        h ^= c;
        h *= 1099511628211ull;
      }
      return h ? h : 1;
    }
    remixapi_Float3D vector3(const Json &v) {
      return {v.at(0).get<float>(), v.at(1).get<float>(), v.at(2).get<float>()};
    }
    remixapi_InstanceInfoBlendEXT blend_state(const Json &values) {
      auto data = capture_alpha_defaults();
      data.update(values);
      remixapi_InstanceInfoBlendEXT state{};
      state.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      state.alphaTestEnabled = data.at("alphaTestEnabled").get<bool>();
      state.alphaTestReferenceValue = data.at("alphaTestReferenceValue").get<uint8_t>();
      state.alphaTestCompareOp = state.alphaTestEnabled ? data.at("alphaTestCompareOp").get<uint32_t>() : 7u;
      state.alphaBlendEnabled = data.at("alphaBlendEnabled").get<bool>();
      state.srcColorBlendFactor = data.at("srcColorBlendFactor");
      state.dstColorBlendFactor = data.at("dstColorBlendFactor");
      state.colorBlendOp = data.at("colorBlendOp");
      state.srcAlphaBlendFactor = data.at("srcAlphaBlendFactor");
      state.dstAlphaBlendFactor = data.at("dstAlphaBlendFactor");
      state.alphaBlendOp = data.at("alphaBlendOp");
      state.writeMask = data.at("writeMask");
      state.textureColorArg1Source = data.at("textureColorArg1Source");
      state.textureColorArg2Source = data.at("textureColorArg2Source");
      state.textureColorOperation = data.at("textureColorOperation");
      state.textureAlphaArg1Source = data.at("textureAlphaArg1Source");
      state.textureAlphaArg2Source = data.at("textureAlphaArg2Source");
      state.textureAlphaOperation = data.at("textureAlphaOperation");
      state.tFactor = data.at("tFactor");
      state.isTextureFactorBlend = data.at("isTextureFactorBlend").get<bool>();
      state.isVertexColorBakedLighting = data.at("isVertexColorBakedLighting").get<bool>();
      return state;
    }
    remixapi_Transform transform(const Json &values) {
      if (values.size() != 16)
        throw std::runtime_error("Expected 16 transform elements");
      remixapi_Transform result{};
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
          result.matrix[r][c] = values.at(c * 4 + r).get<float>();
      return result;
    }
#ifdef _WIN32
    using NativePath = std::wstring;
    NativePath native_path(const std::string &value) { return utf8Path(value).wstring(); }
#else
    using NativePath = std::string;
    NativePath native_path(const std::string &value) { return value; }
#endif

  } // namespace
  remixapi_Transform transformed(const remixapi_Transform &delta, const remixapi_Transform &original) {
    remixapi_Transform result{};
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c) {
        for (int k = 0; k < 3; ++k)
          result.matrix[r][c] += delta.matrix[r][k] * original.matrix[k][c];
        if (c == 3)
          result.matrix[r][c] += delta.matrix[r][3];
      }
    return result;
  }
  Submission::Submission(NativeResources &resources, const Snapshot &snapshot, uint64_t namespaceId, bool applyMods)
      : resources_(resources), namespace_(namespaceId), applyMods_(applyMods),
        snapshot_({{"cameras", snapshot.scene["cameras"]}}) {
    try {
      for (const auto &[path, texture] : snapshot.textures)
        resources_.cacheTexturePath(path, texture);
      material({{"id", "default"}, {"albedo", {.7, .7, .7}}});
      for (const auto &m : snapshot.scene["materials"])
        material_defs_.emplace(m.at("id").get<std::string>(), m);
      for (const auto &m : snapshot.scene["meshes"])
        mesh(m);
      for (const auto &l : snapshot.scene["lights"])
        add_light(l);
    } catch (...) {
      clear();
      throw;
    }
  }
  namespace {
    // A cheap digest of a surface's arrays, word by word.
    uint64_t mixBytes(const void *bytes, size_t size, uint64_t h) {
      const auto *data = static_cast<const uint8_t *>(bytes);
      for (; size >= 8; size -= 8, data += 8) {
        uint64_t word;
        std::memcpy(&word, data, 8);
        h = (h ^ word) * 0x9e3779b97f4a7c15ull;
        h ^= h >> 29;
      }
      for (; size; --size, ++data)
        h = (h ^ *data) * 1099511628211ull;
      return h;
    }
  } // namespace
  void PreparedMeshes::add(ComposedMesh &&composed, std::mutex &mutex) {
    PreparedMesh mesh;
    mesh.id = std::move(composed.id);
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c)
        mesh.transform.matrix[r][c] = float(composed.transform[c * 4 + r]);
    mesh.doubleSided = composed.doubleSided;
    mesh.alphaState = std::move(composed.alphaState);
    // The surfaces of each category flags, as Submission::mesh groups them.
    std::map<uint32_t, std::vector<size_t>> groups;
    for (size_t i = 0; i < composed.surfaces.size(); ++i)
      if (!composed.surfaces[i].vertices.empty())
        groups[composed.surfaces[i].categoryFlags].push_back(i);
    for (const auto &[flags, members] : groups) {
      // Hidden and ignored geometry is never submitted (mesh_group).
      if (flags & (REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE))
        continue;
      std::vector<std::pair<std::vector<remixapi_HardcodedVertex>, std::vector<uint32_t>>> welded;
      std::vector<std::vector<uint32_t>> joints;
      std::vector<std::vector<float>> weights;
      PreparedMesh::Part part;
      part.flags = flags;
      uint64_t key = flags + 1;
      for (size_t member : members) {
        auto &source = composed.surfaces[member];
        const size_t corners = source.vertices.size() / 8;
        const uint32_t influences = source.influences;
        if (!source.colors.empty() && source.colors.size() != corners)
          throw std::runtime_error("Invalid mesh vertex colors");
        // Identical corners become one vertex: what USD's faces share, which
        // compose_scene wrote out once a corner.
        std::vector<uint32_t> table(std::max<size_t>(8, std::bit_ceil(corners * 2)), UINT32_MAX);
        const size_t mask = table.size() - 1;
        welded.emplace_back();
        auto &[vertices, indices] = welded.back();
        vertices.reserve(corners / 2 + 1);
        indices.reserve(corners);
        std::vector<uint32_t> firstCorner;
        firstCorner.reserve(corners / 2 + 1);
        joints.emplace_back();
        weights.emplace_back();
        for (size_t corner = 0; corner < corners; ++corner) {
          const float *v = source.vertices.data() + corner * 8;
          const uint32_t *boneIndex = influences ? source.jointIndices.data() + corner * influences : nullptr;
          const float *boneWeight = influences ? source.jointWeights.data() + corner * influences : nullptr;
          const uint32_t color = source.colors.empty() ? 0xffffffffu : source.colors[corner];
          uint64_t h = mixBytes(v, sizeof(float) * 8, color + 1);
          if (influences) {
            h = mixBytes(boneIndex, influences * sizeof(uint32_t), h);
            h = mixBytes(boneWeight, influences * sizeof(float), h);
          }
          size_t slot = h & mask;
          uint32_t index = UINT32_MAX;
          for (;; slot = (slot + 1) & mask) {
            const uint32_t held = table[slot];
            if (held == UINT32_MAX)
              break;
            const uint32_t other = firstCorner[held];
            if (std::memcmp(v, source.vertices.data() + size_t(other) * 8, sizeof(float) * 8) == 0 &&
                (source.colors.empty() || source.colors[other] == color) &&
                (!influences || (std::memcmp(boneIndex, source.jointIndices.data() + size_t(other) * influences,
                                             influences * sizeof(uint32_t)) == 0 &&
                                 std::memcmp(boneWeight, source.jointWeights.data() + size_t(other) * influences,
                                             influences * sizeof(float)) == 0))) {
              index = held;
              break;
            }
          }
          if (index == UINT32_MAX) {
            index = uint32_t(vertices.size());
            table[slot] = index;
            firstCorner.push_back(uint32_t(corner));
            remixapi_HardcodedVertex vertex{};
            std::copy_n(v, 3, vertex.position);
            std::copy_n(v + 3, 3, vertex.normal);
            // Some captures contain NaN/Inf UVs. Keep the mesh renderable with a
            // zero fallback for just the invalid component.
            for (int i = 0; i < 2; ++i)
              vertex.texcoord[i] = std::isfinite(v[i + 6]) ? v[i + 6] : 0.f;
            // Match Remix's USD importer: captures store V flipped from runtime
            // texture coordinates.
            vertex.texcoord[1] = 1.f - vertex.texcoord[1];
            vertex.color = color;
            vertices.push_back(vertex);
            if (influences) {
              joints.back().insert(joints.back().end(), boneIndex, boneIndex + influences);
              weights.back().insert(weights.back().end(), boneWeight, boneWeight + influences);
            }
          }
          indices.push_back(index);
        }
        // USD uses counter-clockwise fronts; the Remix external mesh API uses clockwise fronts.
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
          std::swap(indices[i + 1], indices[i + 2]);
        for (uint32_t joint : joints.back())
          mesh.bones = std::max(mesh.bones, joint + 1);
        key = mixBytes(source.material.data(), source.material.size(), key);
        key = mixBytes(vertices.data(), vertices.size() * sizeof(remixapi_HardcodedVertex), key);
        key = mixBytes(indices.data(), indices.size() * sizeof(uint32_t), key);
        key = mixBytes(joints.back().data(), joints.back().size() * sizeof(uint32_t), key);
        key = mixBytes(weights.back().data(), weights.back().size() * sizeof(float), key);
        std::vector<float>().swap(source.vertices);
      }
      part.key = key ? key : 1;
      bool seen;
      {
        std::lock_guard lock(mutex);
        seen = parts.contains(part.key);
      }
      if (!seen) {
        std::vector<std::shared_ptr<scene::Surface>> surfaces;
        std::vector<std::string> materials;
        uint64_t size = 0;
        for (size_t i = 0; i < members.size(); ++i) {
          const auto &[vertices, indices] = welded[i];
          remixapi_MeshInfoSurfaceTriangles info{};
          info.vertices_values = vertices.data();
          info.vertices_count = vertices.size();
          info.indices_values = indices.data();
          info.indices_count = indices.size();
          const auto &source = composed.surfaces[members[i]];
          if (!joints[i].empty()) {
            info.skinning_hasvalue = 1;
            info.skinning_value.bonesPerVertex = source.influences;
            info.skinning_value.blendWeights_values = weights[i].data();
            info.skinning_value.blendWeights_count = uint32_t(weights[i].size());
            info.skinning_value.blendIndices_values = joints[i].data();
            info.skinning_value.blendIndices_count = uint32_t(joints[i].size());
          }
          surfaces.push_back(NativeResources::prepareSurface(info, false, size));
          materials.push_back(source.material);
        }
        std::lock_guard lock(mutex);
        if (parts.emplace(part.key, std::pair{std::move(surfaces), std::move(materials)}).second)
          bytes += size;
      }
      {
        std::lock_guard lock(mutex);
        const auto &known = parts.at(part.key);
        part.surfaces = known.first;
        part.materials = known.second;
      }
      mesh.parts.push_back(std::move(part));
    }
    std::lock_guard lock(mutex);
    meshes.push_back(std::move(mesh));
  }
  PreparedMod prepareMod(const Snapshot &snapshot, PreparedMeshes &&meshes, PreparedTextures &&textures) {
    PreparedMod mod;
    mod.meshes = std::move(meshes);
    mod.textures = std::move(textures);
    // Composed in parallel: instances number the same in every load.
    std::sort(mod.meshes.meshes.begin(), mod.meshes.meshes.end(),
              [](const PreparedMesh &a, const PreparedMesh &b) { return a.id < b.id; });
    for (const auto &m : snapshot.scene["materials"])
      mod.materials.emplace(m.at("id").get<std::string>(), m);
    for (const auto &entry : snapshot.scene["replacements"]["meshes"])
      mod.groups[entry.at("path").get<std::string>()];
    for (const auto &entry : snapshot.scene["replacements"]["lights"])
      mod.groups[entry.at("path").get<std::string>()];
    for (const auto &root : mod.meshes.failed)
      if (auto group = mod.groups.find(root); group != mod.groups.end())
        group->second.failed = true;
    for (const auto &l : snapshot.scene["lights"])
      if (auto group = mod.groups.find(replacementRoot(l.value("id", std::string()))); group != mod.groups.end()) {
        group->second.lights.push_back(mod.lights.size());
        mod.lights.push_back(l);
      }
    mod.streamed.reserve(mod.materials.size());
    for (const auto &[id, def] : mod.materials) {
      PreparedMod::Streamed stream;
      stream.id = id;
      for (auto &path : materialTextureFiles(def))
        if (mod.textures.previews.contains(path))
          stream.paths.push_back(std::move(path));
      mod.streamed.push_back(std::move(stream));
    }
    return mod;
  }
  Submission::Submission(NativeResources &resources, PreparedMod &&mod, uint64_t namespaceId)
      : resources_(resources), namespace_(namespaceId), applyMods_(false), snapshot_({{"cameras", Json::array()}}) {
    prepared_ = true;
    prepared_meshes_ = std::move(mod.meshes);
    previews_ = std::move(mod.textures.previews);
    whole_ = std::move(mod.textures.whole);
    material_defs_ = std::move(mod.materials);
    groups_ = std::move(mod.groups);
    light_origins_ = std::move(mod.lights);
    streamed_ = std::move(mod.streamed);
    // Kept for the draws asking for them by path (create_light, overrideLight).
    for (const auto &[path, texture] : whole_)
      resources_.cacheTexturePath(path, texture);
  }
  bool Submission::publish(std::chrono::steady_clock::time_point deadline) {
    if (published_)
      return true;
    if (next_material_ == 0 && next_mesh_ == 0 && !materials_.contains("default"))
      material({{"id", "default"}, {"albedo", {.7, .7, .7}}});
    int polled = 0;
    const auto expired = [&] { return (++polled & 3) == 0 && std::chrono::steady_clock::now() >= deadline; };
    for (; next_material_ < streamed_.size(); ++next_material_) {
      if (expired())
        return false;
      auto &stream = streamed_[next_material_];
      try {
        createStreamed(stream, false);
      } catch (...) {
        // What uses it stays the game's (publishMesh; the material's replacement
        // is left out).
      }
    }
    auto &meshes = prepared_meshes_.meshes;
    for (; next_mesh_ < meshes.size(); ++next_mesh_) {
      if (expired())
        return false;
      publishMesh(meshes[next_mesh_]);
    }
    prepared_meshes_ = {};
    published_ = true;
    return true;
  }
  void Submission::publishMesh(PreparedMesh &mesh) {
    auto group = groups_.find(replacementRoot(mesh.id));
    if (group == groups_.end())
      return;
    if (mesh.parts.empty())
      return;
    try {
      for (auto &part : mesh.parts) {
        auto cached = mesh_cache_.find(part.key);
        if (cached == mesh_cache_.end()) {
          for (size_t i = 0; i < part.surfaces.size(); ++i) {
            const auto handle = materialHandle(part.materials[i]);
            if (!handle)
              throw std::runtime_error("Unknown material " + part.materials[i]);
            part.surfaces[i]->material = handle;
          }
          remixapi_MeshHandle handle{};
          resources_.adoptMesh(hash64(std::to_string(namespace_) + std::to_string(part.key)), part.surfaces, handle);
          meshes_.push_back(handle);
          cached = mesh_cache_.emplace(part.key, handle).first;
        }
        remixapi_InstanceInfo instance{};
        instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
        instance.mesh = cached->second;
        instance.transform = mesh.transform;
        instance.doubleSided = mesh.doubleSided;
        instance.categoryFlags = part.flags;
        group->second.instances.push_back(instances_.size());
        instances_.push_back(instance);
        instance_hashes_.push_back(0);
        instance_bones_.push_back(mesh.bones);
        blend_states_.push_back(blend_state(mesh.alphaState));
        instance_origins_.emplace_back(mesh.id, instance.transform);
      }
    } catch (...) {
      // The game's mesh keeps drawing.
      group->second.failed = true;
    }
  }
  void Submission::createStreamed(StreamedMaterial &stream, bool full) {
    const auto &def = material_defs_.at(stream.id);
    // The files as this material should have them, only while it is made: a
    // host naming one of them gets it as usual.
    std::vector<std::shared_ptr<scene::Texture>> held;
    for (const auto &path : stream.paths) {
      std::shared_ptr<scene::Texture> texture;
      if (full)
        texture = pulled_[path].lock();
      if (!texture)
        if (auto preview = previews_.find(path); preview != previews_.end())
          texture = preview->second;
      if (texture) {
        resources_.cacheTexturePath(path, texture);
        held.push_back(std::move(texture));
      }
    }
    try {
      material(def);
    } catch (...) {
      for (const auto &path : stream.paths)
        resources_.uncacheTexturePath(path);
      throw;
    }
    for (const auto &path : stream.paths)
      resources_.uncacheTexturePath(path);
    stream.handle = materials_.at(stream.id);
    resources_.setMaterialStreamUse(stream.handle, stream.use);
    if (auto keys = materialKeys_.find(stream.id); keys != materialKeys_.end())
      for (const auto key : keys->second)
        resources_.updateModMaterial(key, stream.handle);
  }
  bool Submission::ready(const std::string &root) const {
    if (!prepared_)
      return true;
    auto it = groups_.find(root);
    return published_ && it != groups_.end() && !it->second.failed;
  }
  void Submission::service(bool pressure) {
    if (!prepared_ || !published_)
      return;
    ++frame_;
    // What was drawn since the last frame.
    for (size_t i = 0; i < streamed_.size(); ++i) {
      auto &stream = streamed_[i];
      if (!*stream.use)
        continue;
      stream.lastUse = frame_;
      if (*stream.use == 1)
        *stream.use = 0;
      if (!stream.full && !stream.queued) {
        stream.queued = true;
        if (stream.paths.empty()) {
          stream.full = true; // No files: it is already all it will be.
          ++materialsInFull_;
        } else
          pull_queue_.push_back(i);
      }
    }
    // Whole files decoded: a few materials a frame take them, so that the
    // renderer's uploads spread over frames.
    if (pull_.valid() && pull_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      auto pull = pull_.get();
      for (size_t i = 0; i < pull.paths.size(); ++i) {
        if (!pull.textures[i])
          continue; // Stays on its low levels.
        pulled_[pull.paths[i]] = pull.textures[i];
        pull_held_.push_back(std::move(pull.textures[i]));
      }
      pull_ready_.insert(pull_ready_.end(), pull.materials.begin(), pull.materials.end());
    }
    const uint64_t uploadBudget = 32ull << 20;
    uint64_t uploaded = 0;
    size_t given = 0;
    for (; given < pull_ready_.size() && uploaded < uploadBudget; ++given) {
      auto &stream = streamed_[pull_ready_[given]];
      for (const auto &path : stream.paths)
        if (auto texture = pulled_[path].lock(); texture && texture->pixels.compressed)
          uploaded += texture->pixels.compressed->upload.size();
      try {
        createStreamed(stream, true);
        stream.full = true;
        ++materialsInFull_;
      } catch (...) {
        stream.full = true; // Stays as it is.
      }
      stream.queued = false;
    }
    pull_ready_.erase(pull_ready_.begin(), pull_ready_.begin() + given);
    if (pull_ready_.empty())
      pull_held_.clear();
    // The next files. A batch stays small: what the camera turns to next waits
    // for no more than one.
    if (!pull_.valid() && pull_ready_.empty() && !pull_queue_.empty()) {
      Pull pull;
      std::set<std::string> seen;
      while (!pull_queue_.empty() && pull.paths.size() < 16) {
        const size_t index = pull_queue_.front();
        pull_queue_.erase(pull_queue_.begin());
        pull.materials.push_back(index);
        for (const auto &path : streamed_[index].paths)
          if (seen.insert(path).second && pulled_[path].expired())
            pull.paths.push_back(path);
      }
      pull_ = std::async(std::launch::async, [pull = std::move(pull)]() mutable {
        pull.textures = decodeTextures(pull.paths);
        return pull;
      });
    }
    // RTX Remix's budget: only when video memory is short, the materials not
    // drawn for a while go back to their low levels; their files stay as long as
    // another material holds them. Scene instances keep theirs.
    if (pressure) {
      const uint64_t idle = 300;
      std::vector<size_t> candidates;
      for (size_t i = 0; i < streamed_.size(); ++i) {
        const auto &stream = streamed_[i];
        if (stream.full && !stream.paths.empty() && *stream.use != 2 && frame_ - stream.lastUse > idle)
          candidates.push_back(i);
      }
      std::sort(candidates.begin(), candidates.end(),
                [&](size_t a, size_t b) { return streamed_[a].lastUse < streamed_[b].lastUse; });
      if (candidates.size() > 16)
        candidates.resize(16);
      for (const size_t i : candidates) {
        auto &stream = streamed_[i];
        try {
          createStreamed(stream, false);
          stream.full = false;
          --materialsInFull_;
        } catch (...) {
        }
      }
    }
  }
  Submission::~Submission() { clear(); }
  void Submission::clear() {
    for (auto mesh : meshes_)
      resources_.destroyMesh(mesh);
    for (auto light : attached_lights_)
      resources_.destroyLight(light);
    for (auto light : lights_)
      resources_.destroyLight(light);
    for (auto [id, material] : materials_)
      resources_.destroyMaterial(material);
  }
  void Submission::retireAttachments() {
    for (auto it = attached_lights_.begin(); it != attached_lights_.end();) {
      if (!used_attachments_.contains(*it)) {
        resources_.destroyLight(*it);
        it = attached_lights_.erase(it);
      } else
        ++it;
    }
    used_attachments_.clear();
  }
  void Submission::draw(bool camera, float aspect, const remixapi_InstanceInfo *parent, const std::string &root,
                        bool meshes, bool lights) {
    if (parent) {
      auto *batch = static_cast<const remixapi_InstanceInfoGpuInstancingEXT *>(
          scene::extension(parent->pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT));
      if (batch) {
        if (batch->instanceTransforms_count > 65536 ||
            (batch->instanceTransforms_count && !batch->instanceTransforms_values))
          throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
        auto *source = static_cast<const remixapi_InstanceIdentityEXT *>(
            scene::extension(parent->pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT));
        for (uint32_t i = 0; i < batch->instanceTransforms_count; ++i) {
          auto instance = *parent;
          instance.transform = transformed(parent->transform, batch->instanceTransforms_values[i]);
          remixapi_InstanceIdentityEXT identity{};
          identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
          identity.instanceId =
              source && source->instanceId ? hash64(std::to_string(source->instanceId) + ":" + std::to_string(i)) : 0;
          instance.pNext = &identity;
          draw(false, aspect, &instance, root, meshes, lights);
        }
        return;
      }
    }
    if (camera && !snapshot_["cameras"].empty()) {
      const auto &source = snapshot_["cameras"][0];
      auto matrix = transform(source["transform"]);
      auto direction = [&](int axis, float sign) {
        float n = 0;
        for (int c = 0; c < 3; ++c)
          n += matrix.matrix[c][axis] * matrix.matrix[c][axis];
        n = std::sqrt(n);
        return remixapi_Float3D{matrix.matrix[0][axis] * sign / n, matrix.matrix[1][axis] * sign / n,
                                matrix.matrix[2][axis] * sign / n};
      };
      remixapi_CameraInfoParameterizedEXT p{};
      p.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
      p.position = {matrix.matrix[0][3], matrix.matrix[1][3], matrix.matrix[2][3]};
      p.right = direction(0, 1);
      p.up = direction(1, 1);
      p.forward = direction(2, -1);
      p.aspect = aspect > 0 ? aspect : 16.f / 9;
      p.fovYInDegrees = 2 *
                        std::atan(source.value("horizontal_aperture", 20.955f) /
                                  (2 * source.value("focal_length", 50.f) * p.aspect)) *
                        180 / 3.14159265359f;
      p.nearPlane = .01f;
      p.farPlane = 1e7f;
      remixapi_CameraInfo camera{};
      camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
      camera.pNext = &p;
      camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
      resources_.setupCamera(camera);
    }
    // The high word is the persistent direct-loader document id; the low word
    // changes on reload. Attachments instead inherit the live host instance id.
    uint64_t parentId = applyMods_ ? namespace_ >> 32 : 0;
    if (parent)
      if (auto *identity = static_cast<const remixapi_InstanceIdentityEXT *>(
              scene::extension(parent->pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT)))
        parentId = identity->instanceId;
    if (parent && !parentId)
      parentId = scene::hashBytes(&parent->transform, sizeof(parent->transform));
    // A replaced host instance the camera does not see inside a mask group's
    // coverage (remixapi_InstanceInfoMaskEXT CUT): so for its replacement.
    remixapi_InstanceInfoMaskEXT cut{};
    if (parent)
      if (auto *mask = static_cast<const remixapi_InstanceInfoMaskEXT *>(
              scene::extension(parent->pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MASK_EXT));
          mask && mask->role == REMIXAPI_INSTANCE_MASK_CUT_EXT)
        cut = *mask;
    // remixapi_InstanceInfoCameraCullingEXT: a replacement stays hidden where
    // the camera cannot see its host instance's cluster. Its meshes have their
    // own winding, so the host's back-face flag does not carry over.
    remixapi_InstanceInfoCameraCullingEXT culling{};
    if (parent)
      if (auto *given = static_cast<const remixapi_InstanceInfoCameraCullingEXT *>(
              scene::extension(parent->pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_CAMERA_CULLING_EXT));
          given && (given->flags & REMIXAPI_INSTANCE_CAMERA_CULL_VISIBILITY_EXT)) {
        culling.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_CAMERA_CULLING_EXT;
        culling.flags = REMIXAPI_INSTANCE_CAMERA_CULL_VISIBILITY_EXT;
      }
    // Prepared, the replacement's own instances and lights.
    const Group *group = nullptr;
    if (prepared_) {
      auto it = groups_.find(root);
      if (it == groups_.end() || !published_ || it->second.failed)
        return;
      group = &it->second;
    }
    const size_t instanceCount = group ? group->instances.size() : instances_.size();
    for (size_t n = 0; meshes && n < instanceCount; ++n) {
      const size_t i = group ? group->instances[n] : n;
      if (!group && !root.empty() && instance_origins_[i].first != root &&
          !instance_origins_[i].first.starts_with(root + "/"))
        continue;
      auto instance = instances_[i];
      instance.pNext = &blend_states_[i];
      // Skinned by the bones of the draw it replaces (dxvk-remix: the replaced
      // draw's skinning state). Bones past those the draw names stay at
      // identity, as D3D9's world matrices start, which a draw without bones
      // leaves in the bind pose.
      remixapi_InstanceInfoBoneTransformsEXT bones{};
      std::vector<remixapi_Transform> palette;
      if (const uint32_t needed = instance_bones_[i]) {
        const auto *given = parent ? static_cast<const remixapi_InstanceInfoBoneTransformsEXT *>(scene::extension(
                                         parent->pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT))
                                   : nullptr;
        bones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
        bones.pNext = instance.pNext;
        if (given && given->boneTransforms_count >= needed) {
          bones.boneTransforms_values = given->boneTransforms_values;
          bones.boneTransforms_count = given->boneTransforms_count;
        } else {
          remixapi_Transform unmoved{};
          for (int c = 0; c < 3; ++c)
            unmoved.matrix[c][c] = 1;
          palette.assign(needed, unmoved);
          if (given)
            std::copy_n(given->boneTransforms_values, given->boneTransforms_count, palette.begin());
          bones.boneTransforms_values = palette.data();
          bones.boneTransforms_count = needed;
        }
        instance.pNext = &bones;
      }
      // What the draw it replaces is (its view model, sky...).
      if (parent)
        instance.categoryFlags |= parent->categoryFlags;
      if (cut.group) {
        cut.pNext = instance.pNext;
        instance.pNext = &cut;
      }
      if (culling.flags) {
        culling.pNext = instance.pNext;
        instance.pNext = &culling;
      }
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.pNext = instance.pNext;
      identity.instanceId = hash64(std::to_string(parentId) + ":" + instance_origins_[i].first + ":" +
                                   std::to_string(instance.categoryFlags));
      identity.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
      identity.rasterVisible = true;
      instance.pNext = &identity;
      if (parent)
        instance.transform = transformed(parent->transform, instance.transform);
      if (!applyMods_ || !modInstance(resources_, instance, instance_hashes_[i]))
        resources_.drawInstance(instance);
    }
    const size_t lightCount = group ? group->lights.size() : lights_.size();
    for (size_t n = 0; lights && n < lightCount; ++n) {
      const size_t i = group ? group->lights[n] : n;
      const auto id = light_origins_[i]["id"].get<std::string>();
      if (!group && !root.empty() && id != root && !id.starts_with(root + "/"))
        continue;
      // Prepared, lights draw attached to a host instance only.
      if (group && !parent)
        continue;
      if (parent) {
        auto data = light_origins_[i];
        data["id"] = std::to_string(parentId) + ":" + id;
        auto handle = create_light(data, transformed(parent->transform, transform(data["transform"])));
        attached_lights_.insert(handle);
        used_attachments_.insert(handle);
        resources_.drawLight(handle);
      } else if (!applyMods_ || !modLight(resources_, lights_[i], light_origins_[i].value("hash", uint64_t(0))))
        resources_.drawLight(lights_[i]);
    }
  }
  remixapi_MaterialHandle Submission::materialHandle(const std::string &id) {
    if (auto it = materials_.find(id); it != materials_.end())
      return it->second;
    // Prepared: publish made every one it could.
    auto def = prepared_ ? material_defs_.end() : material_defs_.find(id);
    if (def == material_defs_.end())
      return nullptr;
    material(def->second);
    return materials_.at(id);
  }
  void Submission::material(const Json &data) {
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.hash = hash64(std::to_string(namespace_) + data.dump());
    auto albedo = native_path(data.value("albedo_texture", std::string()));
    auto normal = native_path(data.value("normal_texture", std::string()));
    auto roughness = native_path(data.value("roughness_texture", std::string()));
    auto metal = native_path(data.value("metallic_texture", std::string()));
    auto emission = native_path(data.value("emissive_texture", std::string()));
    info.albedoTexture = albedo.c_str();
    info.normalTexture = normal.c_str();
    info.emissiveTexture = emission.c_str();
    info.emissiveIntensity = data.value("emissive_intensity", 0.f);
    info.emissiveColorConstant = vector3(data.value("emissive", Json::array({0, 0, 0})));
    info.spriteSheetRow = 1;
    info.spriteSheetCol = 1;
    // Zero-initialized API fields mean nearest/clamp, unlike the MDL defaults.
    info.filterMode = data.value("filter_mode", 1);
    info.wrapModeU = data.value("wrap_mode_u", 1);
    info.wrapModeV = data.value("wrap_mode_v", 1);
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = vector3(data.value("albedo", Json::array({0.7, 0.7, 0.7})));
    opaque.opacityConstant = data.value("opacity", 1.f);
    opaque.roughnessConstant = data.value("roughness", 0.5f);
    opaque.metallicConstant = data.value("metallic", 0.f);
    opaque.roughnessTexture = roughness.c_str();
    opaque.metallicTexture = metal.c_str();
    opaque.alphaTestType = data.value("alpha_test", 7);
    opaque.alphaReferenceValue = data.value("alpha_reference", 128);
    opaque.useDrawCallAlphaState = data.value("use_legacy_alpha", true);
    opaque.blendType_hasvalue = data.value("blend_enabled", false);
    opaque.blendType_value = data.value("blend_type", 0);
    opaque.invertedBlend = data.value("inverted_blend", false);
    remixapi_MaterialInfoTranslucentEXT glass{};
    glass.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    glass.refractiveIndex = data.value("ior", 1.5f);
    glass.transmittanceTexture = albedo.c_str();
    glass.transmittanceColor = vector3(data.value("transmittance", Json::array({1, 1, 1})));
    glass.transmittanceMeasurementDistance = data.value("transmittance_distance", 1.f);
    glass.thinWallThickness_hasvalue = data.value("thin_walled", false);
    glass.thinWallThickness_value = data.value("thin_wall_thickness", 1.f);
    glass.useDiffuseLayer = data.value("use_diffuse_layer", false);
    info.pNext = data.value("kind", std::string("opaque")) == "translucent" ? static_cast<void *>(&glass)
                                                                            : static_cast<void *>(&opaque);
    remixapi_MaterialNormalEncodingEXT normal_encoding{};
    normal_encoding.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT;
    normal_encoding.pNext = info.pNext;
    // AperturePBR's encodings (0 octahedral, 1 tangent space OpenGL, 2 tangent
    // space DirectX) as the runtime's (1 octahedral, 3 OpenGL, 2 DirectX).
    const int encoding = data.value("normal_encoding", 0);
    normal_encoding.encoding = encoding == 1 ? 3 : encoding == 2 ? 2 : 1;
    info.pNext = &normal_encoding;
    // AperturePBR's thin film (scene.cpp material_json), authored values
    // clamped to the extension's ranges.
    remixapi_MaterialIridescenceEXT film{};
    NativePath film_texture;
    // Opaque and translucent materials alike (glass and water with a film).
    if (const auto found = data.find("iridescence"); found != data.end() && info.pNext != nullptr) {
      const auto &f = *found;
      auto value = [&](const char *key, float fallback, float low, float high) {
        const float v = f.value(key, fallback);
        return std::isfinite(v) ? std::clamp(v, low, high) : fallback;
      };
      film.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT;
      film.strength = value("strength", 1, 0, 1);
      film.filmRefractiveIndex = value("ior", 1.5f, 1, 3);
      film.thickness = value("thickness", 200, 0, 2000);
      film.thicknessMin = value("min", 100, 0, 2000);
      film.thicknessMax = std::max(film.thicknessMin, value("max", 800, 0, 2000));
      film.flowAmplitude = value("flow_amplitude", 0, 0, 2000);
      film.flowScale = value("flow_scale", 1, 1e-3f, 1e6f);
      film.flowSpeed = value("flow_speed", .1f, -100, 100);
      film.structural = value("structural", 0, 0, 1);
      film_texture = native_path(f.value("texture", std::string()));
      film.thicknessTexture = film_texture.c_str();
      opaque.alphaIsThinFilmThickness = f.value("from_alpha", false);
      film.pNext = info.pNext;
      info.pNext = &film;
    }
    // RTX Remix's subsurface inputs (opaque materials), and OpenRemix's weight.
    remixapi_MaterialInfoOpaqueSubsurfaceEXT subsurface{};
    remixapi_MaterialSubsurfaceWeightEXT subsurface_weight{};
    NativePath sss_transmittance, sss_thickness, sss_single_scattering, sss_radius;
    if (const auto found = data.find("subsurface");
        found != data.end() && info.pNext != nullptr && data.value("kind", std::string("opaque")) != "translucent") {
      const auto &f = *found;
      const auto number = [&](const char *key, float fallback) {
        const auto v = f.value(key, Json(fallback));
        const float x = v.is_number() ? v.get<float>() : fallback;
        return std::isfinite(x) ? x : fallback;
      };
      const auto color = [&](const char *key, float fallback) {
        const auto v = f.value(key, Json::array());
        const auto at = [&](size_t i) {
          const float x = v.size() == 3 && v[i].is_number() ? v[i].get<float>() : fallback;
          return std::isfinite(x) ? x : fallback;
        };
        return remixapi_Float3D{at(0), at(1), at(2)};
      };
      subsurface.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT;
      subsurface.subsurfaceDiffusionProfile = f.value("diffusion_profile", false);
      subsurface.subsurfaceRadius = color("radius", .5f);
      subsurface.subsurfaceRadiusScale = number("radius_scale", 1);
      subsurface.subsurfaceMaxSampleRadius = number("max_sample_radius", 16);
      subsurface.subsurfaceVolumetricAnisotropy = number("anisotropy", 0);
      subsurface.subsurfaceTransmittanceColor = color("transmittance", 1);
      subsurface.subsurfaceMeasurementDistance = number("measurement_distance", 0);
      subsurface.subsurfaceSingleScatteringAlbedo = color("single_scattering_albedo", .5f);
      sss_transmittance = native_path(f.value("transmittance_texture", std::string()));
      sss_thickness = native_path(f.value("thickness_texture", std::string()));
      sss_single_scattering = native_path(f.value("single_scattering_texture", std::string()));
      sss_radius = native_path(f.value("radius_texture", std::string()));
      subsurface.subsurfaceTransmittanceTexture = sss_transmittance.c_str();
      subsurface.subsurfaceThicknessTexture = sss_thickness.c_str();
      subsurface.subsurfaceSingleScatteringAlbedoTexture = sss_single_scattering.c_str();
      subsurface.subsurfaceRadiusTexture = sss_radius.c_str();
      subsurface.pNext = const_cast<void *>(static_cast<const void *>(info.pNext));
      subsurface_weight.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SUBSURFACE_WEIGHT_EXT;
      subsurface_weight.pNext = &subsurface;
      subsurface_weight.weight = std::clamp(number("weight", 1), 0.f, 1.f);
      info.pNext = &subsurface_weight;
    }
    remixapi_MaterialHandle handle{};
    resources_.createMaterial(info, handle);
    if (applyMods_)
      resources_.setMaterialSourceHash(handle, data.value("hash", uint64_t(0)));
    materials_.emplace(data.at("id").get<std::string>(), handle);
  }
  void Submission::mesh(const Json &data) {
    const auto groups = category_surface_groups(data);
    if (groups.size() == 1) {
      mesh_group(data, groups.begin()->first);
      return;
    }
    for (const auto &[flags, indices] : groups) {
      // Keep the USD id on every group so selection transform previews still
      // move all material subsets together.
      Json group = Json::object();
      for (auto item = data.begin(); item != data.end(); ++item)
        if (item.key() != "surfaces")
          group[item.key()] = item.value();
      group["surfaces"] = Json::array();
      for (auto index : indices)
        group["surfaces"].push_back(data.at("surfaces").at(index));
      mesh_group(group, flags);
    }
  }
  void Submission::mesh_group(const Json &data, uint32_t flags) {
    // Keep hidden/ignored geometry in the editor snapshot, but do not submit
    // it. The native runtime can allocate a zero-sized material buffer when
    // its first frame contains only hidden surfaces.
    if (flags & (REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE))
      return;
    const auto geometry_hash = hash64(data.at("surfaces").dump());
    if (auto cached = mesh_cache_.find(geometry_hash); cached != mesh_cache_.end()) {
      add_instance(data, cached->second, flags);
      return;
    }
    std::vector<std::vector<remixapi_HardcodedVertex>> vertices;
    std::vector<std::vector<uint32_t>> indices, jointIndices;
    std::vector<std::vector<float>> jointWeights;
    std::vector<remixapi_MeshInfoSurfaceTriangles> surfaces;
    for (const auto &surface : data.at("surfaces")) {
      vertices.emplace_back();
      indices.push_back(surface.at("indices").get<std::vector<uint32_t>>());
      if (surface.contains("influences")) {
        jointIndices.push_back(surface.at("joint_indices").get<std::vector<uint32_t>>());
        jointWeights.push_back(surface.at("joint_weights").get<std::vector<float>>());
      } else {
        jointIndices.emplace_back();
        jointWeights.emplace_back();
      }
      const auto colors = surface.value("colors", Json::array());
      if (!colors.empty() && colors.size() != surface.at("vertices").size())
        throw std::runtime_error("Invalid mesh vertex colors");
      for (const auto &v : surface.at("vertices")) {
        if (v.size() != 8)
          throw std::runtime_error("Invalid mesh vertex");
        remixapi_HardcodedVertex vertex{};
        for (int i = 0; i < 3; ++i) {
          vertex.position[i] = v[i].get<float>();
          vertex.normal[i] = v[i + 3].get<float>();
        }
        // Some captures contain NaN/Inf UVs, serialized as JSON null. Keep the
        // mesh renderable with a zero fallback for just the invalid component.
        for (int i = 0; i < 2; ++i) {
          const auto uv = v[i + 6].is_null() ? 0.f : v[i + 6].get<float>();
          vertex.texcoord[i] = std::isfinite(uv) ? uv : 0.f;
        }
        // Match Remix's USD importer: captures store V flipped from runtime texture coordinates.
        vertex.texcoord[1] = 1.f - vertex.texcoord[1];
        vertex.color = colors.empty() ? 0xffffffffu : colors.at(vertices.back().size()).get<uint32_t>();
        vertices.back().push_back(vertex);
      }
      for (auto index : indices.back())
        if (index >= vertices.back().size())
          throw std::runtime_error("Mesh index is out of bounds");
      // USD uses counter-clockwise fronts; the Remix external mesh API uses clockwise fronts.
      for (size_t i = 0; i + 2 < indices.back().size(); i += 3)
        std::swap(indices.back()[i + 1], indices.back()[i + 2]);
      remixapi_MeshInfoSurfaceTriangles info{};
      info.vertices_values = vertices.back().data();
      info.vertices_count = vertices.back().size();
      info.indices_values = indices.back().data();
      info.indices_count = indices.back().size();
      if (!jointIndices.back().empty()) {
        info.skinning_hasvalue = 1;
        info.skinning_value.bonesPerVertex = surface.at("influences").get<uint32_t>();
        info.skinning_value.blendWeights_values = jointWeights.back().data();
        info.skinning_value.blendWeights_count = uint32_t(jointWeights.back().size());
        info.skinning_value.blendIndices_values = jointIndices.back().data();
        info.skinning_value.blendIndices_count = uint32_t(jointIndices.back().size());
      }
      info.material = materialHandle(surface.at("material").get<std::string>());
      if (!info.material)
        throw std::runtime_error("Unknown material " + surface.at("material").get<std::string>());
      surfaces.push_back(info);
    }
    if (surfaces.empty())
      return;
    remixapi_MeshInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash64(std::to_string(namespace_) + std::to_string(geometry_hash));
    info.surfaces_values = surfaces.data();
    info.surfaces_count = uint32_t(surfaces.size());
    remixapi_MeshHandle handle{};
    resources_.createMesh(info, handle);
    meshes_.push_back(handle);
    mesh_cache_[geometry_hash] = handle;
    add_instance(data, handle, flags);
  }
  void Submission::add_instance(const Json &data, remixapi_MeshHandle handle, uint32_t flags) {
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.mesh = handle;
    instance.transform = transform(data.at("transform"));
    instance.doubleSided = data.value("double_sided", false);
    instance.categoryFlags = flags;
    instances_.push_back(instance);
    instance_hashes_.push_back(data.value("hash", uint64_t(0)));
    uint32_t bones = 0;
    for (const auto &surface : data.at("surfaces"))
      for (const auto &joint : surface.value("joint_indices", Json::array()))
        bones = std::max(bones, joint.get<uint32_t>() + 1);
    instance_bones_.push_back(bones);
    blend_states_.push_back(blend_state(data.value("alpha_state", Json::object())));
    instance_origins_.emplace_back(data.at("id").get<std::string>(), instance.transform);
  }
  void Submission::add_light(const Json &data) {
    if (auto light = create_light(data, transform(data.at("transform")))) {
      lights_.push_back(light);
      light_origins_.push_back(data);
    }
  }
  remixapi_LightHandle Submission::create_light(const Json &data, const remixapi_Transform &matrix) {
    remixapi_LightInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    info.hash = hash64(std::to_string(namespace_) + data.at("id").get<std::string>() + ":" +
                       data.at("type").get<std::string>());
    // The fork deliberately ignores updates to sleeping static lights. Editor
    // lights must remain movable after any number of preview/inspector edits.
    info.isDynamic = true;
    remixapi_LightInfoUSDEXT usd{};
    usd.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT;
    const std::map<std::string, remixapi_StructType> types{
        {"SphereLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT},
        {"RectLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT},
        {"DiskLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT},
        {"CylinderLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT},
        {"DistantLight", REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT}};
    const auto type = data.at("type").get<std::string>();
    auto color = vector3(data.at("color"));
    float intensity = data.value("intensity", 1.f) * lightScale_, radius = data.value("radius", 0.5f),
          width = data.value("width", 1.f);
    float height = data.value("height", 1.f), length = data.value("length", 1.f);
    float angle = data.value("angle", 0.53f) * float(3.141592653589793 / 180.0);
    float cone = data.value("cone_angle", 180.f) * float(3.141592653589793 / 180.0);
    float softness = data.value("cone_softness", 0.f), focus = data.value("focus", 0.f);
    remixapi_LightInfoDomeEXT dome{};
    dome.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT;
    auto texture = native_path(data.value("texture", std::string()));
    if (type == "DomeLight") {
      dome.transform = matrix;
      dome.colorTexture = texture.c_str();
      info.pNext = &dome;
      info.radiance = {color.x * intensity, color.y * intensity, color.z * intensity};
    } else {
      auto found = types.find(type);
      if (found == types.end())
        return {};
      usd.lightType = found->second;
      usd.transform = matrix;
      usd.pColor = &color;
      usd.pIntensity = &intensity;
      usd.pRadius = &radius;
      usd.pWidth = &width;
      usd.pHeight = &height;
      usd.pLength = &length;
      usd.pAngleRadians = &angle;
      usd.pConeAngleRadians = &cone;
      info.pNext = &usd;
      usd.pConeSoftness = &softness;
      usd.pFocus = &focus;
    }
    // An IES profile (UsdLuxShapingAPI's file) and a light cookie, chained in
    // front of the light's own description.
    std::string profile;
    remixapi_LightInfoIesProfileEXT ies{};
    ies.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_IES_PROFILE_EXT;
    if (const auto file = data.value("ies", std::string());
        !file.empty() && type != "DomeLight" && type != "DistantLight") {
      std::ifstream input(utf8Path(file), std::ios::binary);
      profile.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
      if (!input && !input.eof())
        throw std::runtime_error("Could not read IES profile " + file);
      ies.data = profile.data();
      ies.size = profile.size();
      ies.pNext = info.pNext;
      info.pNext = &ies;
    }
    auto cookieTexture = native_path(data.value("cookie", std::string()));
    remixapi_LightInfoCookieEXT cookie{};
    cookie.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_COOKIE_EXT;
    if (!cookieTexture.empty() && type == "SphereLight" && data.value("cookie_fov", 0.f) > 0) {
      cookie.texture = cookieTexture.c_str();
      cookie.up = vector3(data.value("cookie_up", Json::array({0, 0, 1})));
      cookie.fovDegrees = data.value("cookie_fov", 0.f);
      cookie.pNext = info.pNext;
      info.pNext = &cookie;
    }
    remixapi_LightHandle light{};
    resources_.createLight(info, light);
    return light;
  }

} // namespace remix::usd
