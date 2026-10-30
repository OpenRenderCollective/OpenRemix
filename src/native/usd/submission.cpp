#include "usd/submission.h"
#include "usd/alpha_state.h"
#include "usd/mods.h"
#include "usd/render_categories.h"
#include "platform/utf8_path.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
namespace remix::usd {
  namespace {
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
#ifdef _WIN32
    using NativePath = std::wstring;
    NativePath native_path(const std::string &value) { return utf8Path(value).wstring(); }
#else
    using NativePath = std::string;
    NativePath native_path(const std::string &value) { return value; }
#endif

  } // namespace
  Submission::Submission(NativeResources &resources, const Snapshot &snapshot, uint64_t namespaceId,
                         bool applyMods)
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
  Submission::Submission(NativeResources &resources, Snapshot &snapshot, uint64_t namespaceId, OnDemand)
      : resources_(resources), namespace_(namespaceId), applyMods_(false),
        snapshot_({{"cameras", Json::array()}}) {
    onDemand_ = true;
    deferred_ = snapshot.deferred;
    // The replacement a prim belongs to: /RootNode/meshes/mesh_<hash>.
    auto rootOf = [](const std::string &id) {
      static const std::string meshes = "/RootNode/meshes/";
      return id.starts_with(meshes) ? id.substr(0, id.find('/', meshes.size())) : std::string();
    };
    material({{"id", "default"}, {"albedo", {.7, .7, .7}}});
    for (const auto &m : snapshot.scene["materials"])
      material_defs_.emplace(m.at("id").get<std::string>(), m);
    for (const auto &entry : snapshot.scene["replacements"]["meshes"])
      groups_[entry.at("path").get<std::string>()].material = entry.value("material", std::string());
    for (auto &m : snapshot.scene["meshes"])
      if (auto group = groups_.find(rootOf(m.value("id", std::string()))); group != groups_.end()) {
        group->second.meshes.push_back(pending_meshes_.size());
        pending_meshes_.push_back(std::move(m));
      }
    snapshot.scene["meshes"] = Json::array();
    for (const auto &l : snapshot.scene["lights"])
      if (auto group = groups_.find(rootOf(l.value("id", std::string()))); group != groups_.end()) {
        group->second.lights.push_back(light_origins_.size());
        light_origins_.push_back(l);
      }
  }
  Submission::~Submission() {
    if (batch_.valid())
      batch_.wait();
    clear();
  }
  bool Submission::ready(const std::string &root) {
    if (!onDemand_)
      return true;
    auto it = groups_.find(root);
    if (it == groups_.end())
      return false;
    auto &group = it->second;
    if (group.state == Group::State::Idle) {
      group.state = Group::State::Requested;
      requests_.push_back(root);
    }
    return group.state == Group::State::Ready;
  }
  std::vector<std::string> Submission::groupTextures(const Group &group) const {
    std::set<std::string> ids;
    for (auto index : group.meshes)
      for (const auto &surface : pending_meshes_[index].value("surfaces", Json::array()))
        ids.insert(surface.value("material", std::string("default")));
    if (!group.material.empty())
      ids.insert(group.material);
    std::vector<std::string> paths;
    for (const auto &id : ids)
      if (!materials_.contains(id))
        for (auto &path : materialTextures(id))
          paths.push_back(std::move(path));
    for (auto index : group.lights)
      for (const char *key : {"texture", "cookie"})
        if (auto path = light_origins_[index].value(key, std::string()); !path.empty())
          paths.push_back(std::move(path));
    return paths;
  }
  void Submission::realize(Group &group) {
    instance_sink_ = &group.instances;
    try {
      for (auto index : group.meshes)
        mesh(pending_meshes_[index]);
      if (!group.material.empty())
        materialHandle(group.material);
    } catch (...) {
      instance_sink_ = nullptr;
      throw;
    }
    instance_sink_ = nullptr;
    // The geometry is on the GPU now.
    for (auto index : group.meshes)
      pending_meshes_[index] = Json();
  }
  void Submission::service() {
    if (!onDemand_)
      return;
    if (batch_.valid()) {
      if (batch_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
      // The batch keeps its textures alive until the materials using them
      // exist (the path cache holds them weakly).
      auto batch = batch_.get();
      // Composed replacements: their meshes and the materials they use.
      for (size_t i = 0; i < batch.composed.size(); ++i) {
        auto &group = groups_.at(batch.roots[i]);
        for (auto &m : batch.composed[i]["materials"])
          material_defs_.try_emplace(m.at("id").get<std::string>(), m);
        for (auto &m : batch.composed[i]["meshes"]) {
          group.meshes.push_back(pending_meshes_.size());
          pending_meshes_.push_back(std::move(m));
        }
      }
      std::set<std::string> failed;
      for (size_t i = 0; i < batch.paths.size(); ++i)
        if (batch.textures[i])
          resources_.cacheTexturePath(batch.paths[i], batch.textures[i]);
        else
          failed.insert(batch.paths[i]);
      // A texture that does not decode is left out where it is used, as RTX
      // Remix leaves it (loader.cpp strictAssets).
      if (!failed.empty()) {
        auto drop = [&](Json &object, const std::string &key) {
          if (object.is_object() && object.contains(key) && object[key].is_string() &&
              failed.contains(object[key].get<std::string>()))
            object[key] = "";
        };
        for (auto &[id, def] : material_defs_) {
          std::vector<std::string> keys;
          for (auto it = def.begin(); it != def.end(); ++it)
            if (it.key().ends_with("_texture"))
              keys.push_back(it.key());
          for (const auto &key : keys)
            drop(def, key);
          if (def.contains("iridescence"))
            drop(def["iridescence"], "texture");
        }
        for (auto &light : light_origins_) {
          drop(light, "texture");
          drop(light, "cookie");
        }
      }
      for (const auto &root : batch.roots) {
        auto &group = groups_.at(root);
        try {
          realize(group);
          group.state = Group::State::Ready;
        } catch (...) {
          // The game's mesh keeps drawing.
          group.state = Group::State::Failed;
        }
      }
    }
    if (requests_.empty())
      return;
    Batch batch;
    batch.roots = std::exchange(requests_, {});
    std::set<std::string> seen;
    for (const auto &root : batch.roots) {
      auto &group = groups_.at(root);
      group.state = Group::State::Loading;
      for (auto &path : groupTextures(group))
        if (seen.insert(path).second)
          batch.paths.push_back(std::move(path));
    }
    for (const auto &[id, handle] : materials_)
      batch.known.insert(id);
    batch_ = std::async(std::launch::async, [batch = std::move(batch), deferred = deferred_]() mutable {
      if (deferred) {
        std::set<std::string> seen(batch.paths.begin(), batch.paths.end());
        for (const auto &root : batch.roots) {
          Json composed = Json::object();
          try {
            composed = composeReplacement(*deferred, root);
          } catch (...) {
            // Left as it is: the game's mesh keeps drawing.
          }
          for (const auto &m : composed.value("materials", Json::array()))
            if (!batch.known.contains(m.value("id", std::string())))
              for (auto &path : materialTextureFiles(m))
                if (seen.insert(path).second)
                  batch.paths.push_back(std::move(path));
          batch.composed.push_back(std::move(composed));
        }
      }
      batch.textures = decodeTextures(batch.paths);
      return batch;
    });
  }
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
  void Submission::draw(bool camera, float aspect, const remixapi_InstanceInfo *parent,
                        const std::string &root, bool meshes, bool lights) {
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
          identity.instanceId = source && source->instanceId
                                    ? hash64(std::to_string(source->instanceId) + ":" + std::to_string(i))
                                    : 0;
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
    // On demand, the replacement's own instances and lights (once created).
    const Group *group = nullptr;
    if (onDemand_) {
      auto it = groups_.find(root);
      if (it == groups_.end() || it->second.state != Group::State::Ready)
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
      // On demand, lights draw attached to a host instance only.
      if (group && !parent)
        continue;
      if (parent) {
        auto data = light_origins_[i];
        data["id"] = std::to_string(parentId) + ":" + id;
        auto handle = create_light(data, transformed(parent->transform, transform(data["transform"])));
        attached_lights_.insert(handle);
        used_attachments_.insert(handle);
        resources_.drawLight(handle);
      } else if (!applyMods_ ||
                 !modLight(resources_, lights_[i], light_origins_[i].value("hash", uint64_t(0))))
        resources_.drawLight(lights_[i]);
    }
  }
  remixapi_MaterialHandle Submission::materialHandle(const std::string &id) {
    if (auto it = materials_.find(id); it != materials_.end())
      return it->second;
    auto def = material_defs_.find(id);
    if (def == material_defs_.end())
      return nullptr;
    material(def->second);
    return materials_.at(id);
  }
  std::vector<std::string> Submission::materialTextures(const std::string &id) const {
    auto def = material_defs_.find(id);
    return def == material_defs_.end() ? std::vector<std::string>() : materialTextureFiles(def->second);
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
    std::vector<std::vector<uint32_t>> indices;
    std::vector<remixapi_MeshInfoSurfaceTriangles> surfaces;
    for (const auto &surface : data.at("surfaces")) {
      vertices.emplace_back();
      indices.push_back(surface.at("indices").get<std::vector<uint32_t>>());
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
    if (instance_sink_)
      instance_sink_->push_back(instances_.size());
    instances_.push_back(instance);
    instance_hashes_.push_back(data.value("hash", uint64_t(0)));
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
    float intensity = data.value("intensity", 1.f), radius = data.value("radius", 0.5f),
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
