#include "scene/images.h"
#include "scene/resources.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace remix {
  using namespace scene;
  using namespace scene::detail;

  void NativeResources::setupCamera(const remixapi_CameraInfo &info) {
    if (info.type < REMIXAPI_CAMERA_TYPE_WORLD || info.type > REMIXAPI_CAMERA_TYPE_VIEW_MODEL) {
      throw invalid;
    }
    Matrix view, projection;
    if (const auto *p = static_cast<const remixapi_CameraInfoParameterizedEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT))) {
      const float vectors[] = {p->position.x, p->position.y, p->position.z, p->forward.x,
                               p->forward.y,  p->forward.z,  p->up.x,       p->up.y,
                               p->up.z,       p->right.x,    p->right.y,    p->right.z};
      finite(vectors, 12);
      if (!std::isfinite(p->aspect) || p->aspect <= 0 || !std::isfinite(p->nearPlane) || p->nearPlane <= 0 ||
          !std::isfinite(p->farPlane) || p->farPlane <= p->nearPlane || !std::isfinite(p->fovYInDegrees) ||
          p->fovYInDegrees <= 0 || p->fovYInDegrees >= 180) {
        throw invalid;
      }
      float basis[3][3] = {{p->right.x, p->right.y, p->right.z},
                           {p->up.x, p->up.y, p->up.z},
                           {p->forward.x, p->forward.y, p->forward.z}};
      for (auto &axis : basis) {
        const float norm = std::hypot(axis[0], axis[1], axis[2]);
        if (!std::isfinite(norm) || norm < 1e-6f) {
          throw invalid;
        }
        for (float &v : axis) {
          v /= norm;
        }
      }
      float inverse[3][3];
      for (int r = 0; r < 3; ++r) {
        const auto &a = basis[(r + 1) % 3], &b = basis[(r + 2) % 3];
        inverse[r][0] = a[1] * b[2] - a[2] * b[1];
        inverse[r][1] = a[2] * b[0] - a[0] * b[2];
        inverse[r][2] = a[0] * b[1] - a[1] * b[0];
      }
      const float det =
          basis[0][0] * inverse[0][0] + basis[0][1] * inverse[0][1] + basis[0][2] * inverse[0][2];
      if (std::abs(det) < 1e-5f) {
        throw invalid;
      }
      const float position[3] = {p->position.x, p->position.y, p->position.z};
      view = identity();
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
          view[c * 4 + r] = inverse[r][c] / det;
          view[12 + r] -= view[c * 4 + r] * position[c];
        }
      }
      const float y = 1 / std::tan(p->fovYInDegrees * 0.00872664625997165f);
      projection = {};
      projection[0] = y / p->aspect;
      projection[5] = y;
      projection[10] = p->farPlane / (p->farPlane - p->nearPlane);
      projection[11] = 1;
      projection[14] = -p->nearPlane * projection[10];
    } else {
      std::memcpy(view.data(), info.view, sizeof(view));
      std::memcpy(projection.data(), info.projection, sizeof(projection));
      finite(view.data(), 16);
      finite(projection.data(), 16);
    }
    auto camera = multiply(projection, view);
    for (int c = 0; c < 4; ++c) {
      camera[c * 4 + 1] = -camera[c * 4 + 1];
    } // Vulkan framebuffer Y.
    finite(camera.data(), 16);
    // Reverse clip depth before multiplying by the view matrix. Subtracting
    // already-rounded clip Z from W in the shader would retain the precision
    // loss that makes nearby layers (such as grass-side overlays) fight.
    std::array<float, 4> reverseDepth{};
    for (int c = 0; c < 4; ++c) {
      double value = 0;
      for (int k = 0; k < 4; ++k)
        value += (double(projection[k * 4 + 3]) - projection[k * 4 + 2]) * view[c * 4 + k];
      reverseDepth[c] = float(value);
    }
    finite(reverseDepth.data(), reverseDepth.size());
    // Validate every extension before committing any camera or world state.
    std::array<float, 3> sky = {};
    if (const auto *environment = static_cast<const remixapi_CameraEnvironmentEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT))) {
      const auto &rgb = environment->radiance;
      sky = {rgb.x, rgb.y, rgb.z};
      finite(sky.data(), sky.size());
      if (environment->enabled > 1 || std::any_of(sky.begin(), sky.end(), [](float v) { return v < 0; }))
        throw invalid;
      if (!environment->enabled)
        sky = {};
    }
    std::array<float, 4> medium = {};
    if (const auto *inside = static_cast<const remixapi_CameraMediumEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_EXT))) {
      const auto &c = inside->transmittanceColor;
      medium = {c.x, c.y, c.z, inside->transmittanceMeasurementDistance};
      finite(medium.data(), medium.size());
      if (inside->inside > 1 || std::any_of(medium.begin(), medium.end(), [](float v) { return v < 0; }))
        throw invalid;
      if (!inside->inside || medium[3] <= 0)
        medium = {};
    }
    if (info.type == REMIXAPI_CAMERA_TYPE_WORLD) {
      m_cameraMedium = medium;
      const auto *metadata = static_cast<const remixapi_CameraTemporalEXT *>(
          extension(info.pNext, REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT));
      m_temporalEpoch =
          metadata ? TemporalEpoch{metadata->worldEpoch, metadata->historyResetEpoch} : TemporalEpoch{};
      m_skyRadiance = sky;
    }
    m_authoredCameras[info.type] = camera;
    m_authoredViews[info.type] = view;
    // Keep the submitted projection before view multiplication loses precision.
    // Recovering it from camera * inverse(view) can alternate between finite
    // and infinite far planes while moving, repeatedly rebuilding reconstruction.
    for (int c = 0; c < 4; ++c)
      projection[c * 4 + 1] = -projection[c * 4 + 1];
    m_projections[info.type] = projection;
    m_authoredReverseDepth[info.type] = reverseDepth;
    m_cameraValid[info.type] = true;

    if (!m_freeCamActive || info.type != REMIXAPI_CAMERA_TYPE_WORLD) {
      m_cameras[info.type] = camera;
      m_views[info.type] = view;
      m_reverseDepth[info.type] = reverseDepth;
    }
  }

  void NativeResources::applyFreeCamera(const scene::FreeCameraState &freeCam) {
    if (!freeCam.enabled) {
      if (m_freeCamActive) {
        m_freeCamActive = false;
        m_cameras = m_authoredCameras;
        m_views = m_authoredViews;
        m_reverseDepth = m_authoredReverseDepth;
      }
      return;
    }
    if (!m_cameraValid[REMIXAPI_CAMERA_TYPE_WORLD]) {
      return;
    }
    m_freeCamActive = true;

    try {
      const auto &baseView = m_authoredViews[REMIXAPI_CAMERA_TYPE_WORLD];
      const auto &baseProjection = m_projections[REMIXAPI_CAMERA_TYPE_WORLD];

      Matrix viewToWorld = scene::inverse(baseView);
      const float upSign = viewToWorld[5] < 0.0f ? -1.0f : 1.0f;
      Matrix rot = getEulerRotationMatrix(upSign * freeCam.pitch, freeCam.yaw);

      Matrix viewToWorldRot = viewToWorld;
      viewToWorldRot[12] = 0.0f;
      viewToWorldRot[13] = 0.0f;
      viewToWorldRot[14] = 0.0f;
      viewToWorldRot[15] = 1.0f;

      Matrix freeCamRot = multiply(viewToWorldRot, rot);

      Matrix freeCamViewToWorld = freeCamRot;
      freeCamViewToWorld[12] = viewToWorld[12] + freeCam.positionOffset[0];
      freeCamViewToWorld[13] = viewToWorld[13] + freeCam.positionOffset[1];
      freeCamViewToWorld[14] = viewToWorld[14] + freeCam.positionOffset[2];
      freeCamViewToWorld[15] = 1.0f;

      Matrix freeCamView = scene::inverse(freeCamViewToWorld);

      Matrix unadjustedProj = baseProjection;
      for (int c = 0; c < 4; ++c) {
        unadjustedProj[c * 4 + 1] = -unadjustedProj[c * 4 + 1];
      }

      Matrix freeCamCamera = multiply(unadjustedProj, freeCamView);
      for (int c = 0; c < 4; ++c) {
        freeCamCamera[c * 4 + 1] = -freeCamCamera[c * 4 + 1];
      }

      std::array<float, 4> freeCamReverseDepth{};
      for (int c = 0; c < 4; ++c) {
        double value = 0;
        for (int k = 0; k < 4; ++k)
          value += (double(unadjustedProj[k * 4 + 3]) - unadjustedProj[k * 4 + 2]) * freeCamView[c * 4 + k];
        freeCamReverseDepth[c] = float(value);
      }

      m_views[REMIXAPI_CAMERA_TYPE_WORLD] = freeCamView;
      m_cameras[REMIXAPI_CAMERA_TYPE_WORLD] = freeCamCamera;
      m_reverseDepth[REMIXAPI_CAMERA_TYPE_WORLD] = freeCamReverseDepth;

      if (m_cameraValid[REMIXAPI_CAMERA_TYPE_SKY]) {
        Matrix skyViewToWorld = freeCamRot;
        Matrix skyView = scene::inverse(skyViewToWorld);
        Matrix skyProj = m_projections[REMIXAPI_CAMERA_TYPE_SKY];
        for (int c = 0; c < 4; ++c) {
          skyProj[c * 4 + 1] = -skyProj[c * 4 + 1];
        }
        Matrix skyCamera = multiply(skyProj, skyView);
        for (int c = 0; c < 4; ++c) {
          skyCamera[c * 4 + 1] = -skyCamera[c * 4 + 1];
        }
        m_views[REMIXAPI_CAMERA_TYPE_SKY] = skyView;
        m_cameras[REMIXAPI_CAMERA_TYPE_SKY] = skyCamera;
      }
    } catch (...) {
      m_cameras[REMIXAPI_CAMERA_TYPE_WORLD] = m_authoredCameras[REMIXAPI_CAMERA_TYPE_WORLD];
      m_views[REMIXAPI_CAMERA_TYPE_WORLD] = m_authoredViews[REMIXAPI_CAMERA_TYPE_WORLD];
      m_reverseDepth[REMIXAPI_CAMERA_TYPE_WORLD] = m_authoredReverseDepth[REMIXAPI_CAMERA_TYPE_WORLD];
    }
  }

  void NativeResources::drawInstance(const remixapi_InstanceInfo &info,
                                     remixapi_MaterialHandle materialOverride) {
    auto mesh = m_meshes.find(key(info.mesh));
    if (mesh == m_meshes.end()) {
      throw invalid;
    }
    InstanceDescription description;
    describeInstance(info, description);
    if (description.hidden) {
      return;
    }
    if (description.traceIdentity && m_frameIds.count(description.instanceId))
      throw invalid;
    const uint32_t count = description.instanced ? description.instanceCount : 1;
    if (uint64_t(count) * mesh->second->surfaces.size() + (description.halo || description.maskSource ? 0 : pendingDrawCount()) > 65536) {
      throw invalid;
    }
    // A USD mod's material wins over remixapi_InstanceInfoMaterialEXT.
    const auto override = materialOverride ? m_materials.at(key(materialOverride)) : overrideMaterial(description);
    m_modelScratch.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      m_modelScratch[i] = description.instanced
                              ? multiply(description.model, transform(description.instanceValues[i]))
                              : description.model;
      finite(m_modelScratch[i].data(), 16);
    }
    std::vector<Draw> pending;
    buildDraws(description, mesh->second, m_modelScratch.data(), count, override, true, pending, nullptr);
    if (!m_hiddenTextures.empty()) {
      for (auto &d : pending)
        applyDrawHidden(d);
    }
    if (description.halo) {
      // A halo source: validated like any instance, kept out of the scene.
      appendHalo(description.haloParameters, std::move(pending));
      return;
    }
    if (description.maskSource) {
      appendMask(description.maskParameters, std::move(pending));
      return;
    }
    if (m_draws.capacity() < m_draws.size() + pending.size())
      m_draws.reserve(std::max(m_draws.size() + pending.size(), m_draws.capacity() * 2));
    if (description.traceIdentity && !pending.empty())
      m_frameIds.insert(description.instanceId);
    noteCut(description, pending);
    m_draws.insert(m_draws.end(), std::make_move_iterator(pending.begin()),
                   std::make_move_iterator(pending.end()));
  }

  // remixapi_InstanceInfoMaterialEXT, shared by DrawInstance and scene
  // instances: every surface is shaded with the
  // override; a null handle, or one that is not a live material, leaves the
  // mesh's own materials.
  std::shared_ptr<Material> NativeResources::overrideMaterial(const InstanceDescription &description) const {
    if (!description.materialOverride)
      return {};
    const auto it = m_materials.find(description.materialOverride);
    return it == m_materials.end() ? nullptr : it->second;
  }

  void NativeResources::appendHalo(const Halo &halo, std::vector<Draw> &&draws, const float *origin) {
    if (draws.empty() || m_haloDraws + draws.size() > maxHaloDraws)
      return;
    auto group = std::find_if(m_halos.begin(), m_halos.end(),
                              [&](const HaloGroup &candidate) { return candidate.halo == halo; });
    if (group == m_halos.end()) {
      if (m_halos.size() >= maxHaloGroups)
        return;
      group = m_halos.insert(m_halos.end(), HaloGroup{halo, {}});
    }
    m_haloDraws += draws.size();
    for (auto &draw : draws) {
      if (origin)
        for (int c = 0; c < 3; ++c)
          draw.model[12 + c] -= origin[c];
      group->draws.push_back(std::move(draw));
    }
  }

  void NativeResources::noteCut(const InstanceDescription &description, const std::vector<Draw> &draws) {
    if (!(description.cutGroup || description.cameraCull) ||
        std::none_of(draws.begin(), draws.end(), [](const Draw &draw) { return draw.traced(); }))
      return;
    if (description.cutGroup)
      m_cutGroups |= 1u << (description.cutGroup - 1);
    m_cameraCull |= description.cameraCull;
  }

  void NativeResources::appendMask(const Mask &mask, std::vector<Draw> &&draws, const float *origin) {
    if (draws.empty() || m_maskDraws + draws.size() > maxMaskDraws)
      return;
    auto run = m_masks.end();
    for (auto it = m_masks.rbegin(); it != m_masks.rend(); ++it) {
      if (it->mask == mask) {
        run = std::prev(it.base());
        break;
      }
      // Joining a run before this one would change what it read or reads.
      if (it->mask.within == mask.group || (mask.within && it->mask.group == mask.within))
        break;
    }
    if (run == m_masks.end()) {
      if (m_masks.size() >= maxMaskRuns)
        return;
      run = m_masks.insert(m_masks.end(), MaskRun{mask, {}});
    }
    m_maskDraws += draws.size();
    for (auto &draw : draws) {
      if (origin)
        for (int c = 0; c < 3; ++c)
          draw.model[12 + c] -= origin[c];
      run->draws.push_back(std::move(draw));
    }
  }

  bool NativeResources::buildDraws(const InstanceDescription &description, const std::shared_ptr<Mesh> &mesh,
                                   const Matrix *models, uint32_t count,
                                   const std::shared_ptr<Material> &materialOverride, bool strict,
                                   std::vector<Draw> &out, std::vector<std::shared_ptr<Material>> *resolved) {
    const auto &d = description;
    if (!m_default) {
      m_default = std::make_shared<Material>();
      m_default->texture = texturePath(nullptr);
    }
    // A mod replaces the material by its own hash (ownKey; 0: unknown), by
    // the key its host named for it (sourceHash: a captured material's hash,
    // or remixapi_MaterialInfoReplacementHashEXT) or by its albedo texture's
    // hash, in that order.
    auto modMaterial = [&](const std::shared_ptr<Material> &original, uint64_t ownKey) -> std::shared_ptr<Material> {
      const uint64_t modKeys[3] = {ownKey, original->sourceHash,
                                   original->texture ? original->texture->getHash() : 0};
      for (const uint64_t modKey : modKeys) {
        if (!modKey)
          continue;
        if (auto replacement = m_modMaterials.find(modKey); replacement != m_modMaterials.end()) {
          const auto &mod = replacement->second;
          // A replacement without an albedo texture (generated PBR mods
          // give normals and roughness only) keeps the albedo of the
          // material it replaces, with that texture's alpha state and
          // sampling, as the capture runtime does; alone it would paint
          // the surface its constant colour.
          // (No albedo texture: the default one every such material gets.)
          if (mod->texture != m_default->texture || original->texture == m_default->texture)
            return mod;
          auto &entry = m_modMerged[ownKey ? ownKey : modKey];
          if (!entry.merged || entry.mod.lock() != mod || entry.original.lock() != original) {
            auto merged = std::make_shared<scene::Material>(*mod);
            merged->texture = original->texture;
            std::copy(std::begin(original->tint), std::end(original->tint), merged->tint);
            merged->compare = original->compare;
            merged->reference = original->reference;
            merged->drawAlpha = original->drawAlpha;
            merged->filter = original->filter;
            merged->wrapU = original->wrapU;
            merged->wrapV = original->wrapV;
            merged->rows = original->rows;
            merged->columns = original->columns;
            merged->fps = original->fps;
            entry = {mod, original, std::move(merged)};
          }
          return entry.merged;
        }
        if (m_modCandidates.contains(modKey) && m_modRequested.insert(modKey).second)
          m_modRequests.push_back(modKey);
      }
      return original;
    };
    bool complete = true;
    for (uint32_t i = 0; i < count; ++i) {
      for (const auto &surface : mesh->surfaces) {
        Draw draw;
        draw.surface = surface;
        draw.mesh = mesh;
        draw.surfaceIndex = uint32_t(&surface - mesh->surfaces.data());
        draw.instanceIndex = i;
        draw.instanceId = d.instanceId;
        draw.classification = d.classification;
        draw.rasterVisible = d.rasterVisible;
        draw.maskGroup = d.maskGroup;
        draw.maskOutside = d.maskOutside;
        draw.cutGroup = d.cutGroup;
        draw.cameraCull = d.cameraCull;
        draw.rasterOrder = d.rasterOrder;
        draw.spriteFlags = d.spriteFlags;
        std::copy_n(d.spriteSoft, 3, draw.spriteSoft);
        // Frames and sequences come from the mesh's sprite vertices.
        if (surface->sprite.empty())
          draw.spriteFlags &= REMIXAPI_SPRITE_SOFT_DEPTH_EXT | REMIXAPI_SPRITE_MODULATE_EXT |
                              REMIXAPI_SPRITE_MODULATE_FADE_EXT;
        draw.model = models[i];
        draw.bones = d.bones;
        if (d.hasRasterState) {
          draw.depth = d.depthTest;
          draw.depthWriteOverride = true;
          draw.depthWrite = d.depthWrite;
          draw.depthCompare = d.depthCompare;
        }
        draw.wireframe = d.wireframe;
        draw.depthBias = (d.categoryFlags & (REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC |
                                             REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC |
                                             REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET)) != 0;
        draw.decal = (d.categoryFlags & (REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC |
                                          REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC |
                                          REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET |
                                          REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_NO_OFFSET)) != 0;
        draw.material = m_default;
        if (surface->material) {
          auto it = m_materials.find(key(surface->material));
          if (it == m_materials.end()) {
            if (strict) {
              throw invalid;
            }
            complete = false;
          } else {
            if (resolved && i == 0)
              resolved->push_back(it->second);
            draw.material = modMaterial(it->second, key(surface->material));
          }
        }
        // An instance's override is replaced as a mesh's own material is: a
        // host that names every draw's material that way (a translation
        // layer whose meshes are shared between materials) has no other.
        if (materialOverride)
          draw.material = modMaterial(materialOverride, d.materialOverride);
        if (surface->bonesPerVertex) {
          if (!d.bones || d.bones->empty()) {
            throw invalid;
          }
          for (auto index : surface->bones) {
            if (index >= d.bones->size()) {
              throw invalid;
            }
          }
        }
        draw.blend = d.blend;
        if (d.hasRasterState && d.disableColorWrites)
          draw.blend.mask = 0;
        draw.blend.enabled |= draw.material->translucent;
        draw.doubleSided = d.doubleSided;
        draw.ignoreAlpha = (d.categoryFlags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL) != 0;
        if (draw.ignoreAlpha) {
          draw.blend.enabled = false;
        }
        draw.alphaOverride = draw.material->drawAlpha && d.hasAlphaState;
        if (draw.alphaOverride && d.alphaTest) {
          draw.compare = d.alphaCompare;
          draw.reference = d.alphaReference;
        }
        if (d.categoryFlags & REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL) {
          draw.camera = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
          // Follows the camera every frame: never part of the static set.
          if (draw.classification == REMIXAPI_INSTANCE_STATIC_EXT)
            draw.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
        } else if (d.categoryFlags & REMIXAPI_INSTANCE_CATEGORY_BIT_SKY) {
          draw.camera = REMIXAPI_CAMERA_TYPE_SKY;
          draw.depth = false;
        }
        out.push_back(std::move(draw));
      }
    }
    return complete;
  }
} // namespace remix

namespace remix::scene {
  using namespace detail;

  void describeInstance(const remixapi_InstanceInfo &info, InstanceDescription &d) {
    d.mesh = key(info.mesh);
    d.model = transform(info.transform);
    d.categoryFlags = info.categoryFlags;
    d.doubleSided = info.doubleSided != 0;
    if (info.categoryFlags & (REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE |
                              REMIXAPI_INSTANCE_CATEGORY_BIT_FIRST_PERSON_PLAYER_SHADOW)) {
      d.hidden = true;
      return;
    }
    if (const auto *override = static_cast<const remixapi_InstanceInfoMaterialEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT)))
      d.materialOverride = key(override->material);
    const auto *alpha =static_cast<const remixapi_InstanceInfoBlendEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT));
    d.blend = blend(alpha);
    const auto *identity = static_cast<const remixapi_InstanceIdentityEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT));
    if (identity && (identity->classification > 2 || identity->rasterVisible > 1 ||
                     (!identity->instanceId && identity->classification != 2)))
      throw invalid;
    if (identity) {
      d.hasIdentity = true;
      d.instanceId = identity->instanceId;
      d.classification = identity->classification;
      d.rasterVisible = identity->rasterVisible != 0;
    }
    d.traceIdentity = identity && identity->classification != REMIXAPI_INSTANCE_RASTER_ONLY_EXT &&
                      !(info.categoryFlags & (REMIXAPI_INSTANCE_CATEGORY_BIT_SKY |
                                              REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL));
    const auto *rasterState = static_cast<const remixapi_InstanceRasterStateEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT));
    if (rasterState && (rasterState->depthTest > 1 || rasterState->depthWrite > 1 ||
                        rasterState->depthCompare > 7 || rasterState->disableColorWrites > 1)) {
      throw invalid;
    }
    if (rasterState) {
      d.hasRasterState = true;
      d.depthTest = rasterState->depthTest != 0;
      d.depthWrite = rasterState->depthWrite != 0;
      d.depthCompare = rasterState->depthCompare;
      d.disableColorWrites = rasterState->disableColorWrites != 0;
    }
    if (alpha && alpha->alphaTestEnabled && alpha->alphaTestCompareOp > 7) {
      throw invalid;
    }
    if (alpha) {
      d.hasAlphaState = true;
      d.alphaTest = alpha->alphaTestEnabled != 0;
      d.alphaCompare = alpha->alphaTestCompareOp;
      d.alphaReference = float(alpha->alphaTestReferenceValue) / 255;
    }
    if (const auto *skin = static_cast<const remixapi_InstanceInfoBoneTransformsEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT))) {
      if (skin->boneTransforms_count > 256 || (skin->boneTransforms_count && !skin->boneTransforms_values)) {
        throw invalid;
      }
      d.bones = std::make_shared<std::vector<Matrix>>();
      for (uint32_t i = 0; i < skin->boneTransforms_count; ++i) {
        d.bones->push_back(transform(skin->boneTransforms_values[i]));
      }
    }
    if (const auto *halo = static_cast<const remixapi_InstanceInfoHaloEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_HALO_EXT))) {
      const float values[] = {halo->color.x, halo->color.y, halo->color.z, halo->blurX, halo->blurY};
      finite(values, 5);
      if (halo->blurX <= 0 || halo->blurY <= 0 || halo->additive > 1 || halo->ignoreZ > 1)
        throw invalid;
      d.halo = true;
      auto &h = d.haloParameters;
      for (int c = 0; c < 3; ++c)
        h.color[c] = std::clamp(values[c], 0.f, 1.f);
      h.blurX = std::min(halo->blurX, Halo::maxBlur);
      h.blurY = std::min(halo->blurY, Halo::maxBlur);
      h.passes = std::clamp(halo->passes, 1u, Halo::maxPasses);
      h.additive = halo->additive != 0;
      h.ignoreZ = halo->ignoreZ != 0;
      // Not a scene instance: no identity, so no duplicate check and no
      // temporal lineage; classification and raster visibility are unused.
      d.traceIdentity = false;
    }
    if (const auto *mask = static_cast<const remixapi_InstanceInfoMaskEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MASK_EXT))) {
      if (!mask->group || mask->group > Mask::maxGroup || mask->role > REMIXAPI_INSTANCE_MASK_CUT_EXT || d.halo)
        throw invalid;
      if (mask->role == REMIXAPI_INSTANCE_MASK_CUT_EXT) {
        // Still the scene instance it was: identity, classification and
        // lineage stay. Only the camera's view of it is limited.
        d.cutGroup = uint8_t(mask->group);
      } else {
        if (mask->role == REMIXAPI_INSTANCE_MASK_SOURCE_EXT) {
          if (mask->within > Mask::maxGroup || mask->within == mask->group || mask->depthTest > 1)
            throw invalid;
          d.maskSource = true;
          d.maskParameters = {mask->group, mask->within, mask->depthTest != 0};
        } else {
          // A raster-only overlay whatever its blend mode and identity say.
          d.maskGroup = uint8_t(mask->group);
          d.maskOutside = mask->role == REMIXAPI_INSTANCE_MASK_OUTSIDE_EXT;
          d.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
        }
        // Neither is a scene instance: no identity, duplicate check or lineage.
        d.traceIdentity = false;
      }
    }
    if (const auto *wire = static_cast<const remixapi_InstanceInfoWireframeEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_WIREFRAME_EXT))) {
      if (wire->flags)
        throw invalid;
      // A raster-only overlay of lines, whatever its identity says.
      d.wireframe = true;
      d.classification = REMIXAPI_INSTANCE_RASTER_ONLY_EXT;
      d.traceIdentity = false;
    }
    if (const auto *culling = static_cast<const remixapi_InstanceInfoCameraCullingEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_CAMERA_CULLING_EXT))) {
      constexpr uint32_t known =
          REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT | REMIXAPI_INSTANCE_CAMERA_CULL_VISIBILITY_EXT;
      if (culling->flags & ~known)
        throw invalid;
      // A scene instance like any other; sources and masked overlays are not
      // in the scene, and a one-sided instance has no back faces left to hide.
      if (!d.halo && !d.maskSource && !d.maskGroup)
        d.cameraCull = uint8_t(culling->flags &
                               (d.doubleSided ? known : uint32_t(REMIXAPI_INSTANCE_CAMERA_CULL_VISIBILITY_EXT)));
    }
    if (const auto *order = static_cast<const remixapi_InstanceInfoRasterOrderEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_RASTER_ORDER_EXT))) {
      // Raster-only overlays that are not masked: a traced instance has no
      // place in the raster order, a masked one keeps its depth-sorted place.
      if (d.classification == REMIXAPI_INSTANCE_RASTER_ONLY_EXT && !d.halo && !d.maskSource && !d.maskGroup)
        d.rasterOrder = order->order;
    }
    if (const auto *sprite = static_cast<const remixapi_InstanceInfoSpriteEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_SPRITE_EXT))) {
      constexpr uint32_t known = 0x1ff;
      const bool soft = (sprite->flags & REMIXAPI_SPRITE_SOFT_DEPTH_EXT) != 0;
      if ((sprite->flags & ~known) || !std::isfinite(sprite->softDistance) || !std::isfinite(sprite->softFadeStart) ||
          !std::isfinite(sprite->softFadeEnd) || (soft && !(sprite->softDistance > 0)))
        throw invalid;
      if (d.classification == REMIXAPI_INSTANCE_RASTER_ONLY_EXT && !d.halo && !d.maskSource && !d.maskGroup) {
        d.spriteFlags = sprite->flags;
        d.spriteSoft[0] = sprite->softDistance;
        d.spriteSoft[1] = sprite->softFadeStart;
        d.spriteSoft[2] = sprite->softFadeEnd;
      }
    }
    const auto *instances = static_cast<const remixapi_InstanceInfoGpuInstancingEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT));
    if (instances) {
      if (instances->instanceTransforms_count && !instances->instanceTransforms_values) {
        throw invalid;
      }
      d.instanced = true;
      d.instanceValues = instances->instanceTransforms_values;
      d.instanceCount = instances->instanceTransforms_count;
    }
  }
} // namespace remix::scene
