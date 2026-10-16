#include "scene/resources.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>

// Scene instances (remixapi_AddSceneInstancesEXT), "retained" here: a draw is
// validated and resolved once, when it is added or re-described, into ready
// Draw records kept in a dense table. drawRetained() then copies the visible
// records into the frame.
//
// Lifetime: an entry names its mesh and materials by handle. It holds the
// objects those handles resolved to, so a DestroyMesh/DestroyMaterial while it
// is registered never leaves a dangling reference. A destroyed or replaced mesh
// or material is marked retired; the entry resolves its handles again at the
// next replay. A mesh that is gone draws nothing until a mesh is created under
// the same hash again. A surface material that is gone falls back to the
// default material, a dead override to the mesh's own material.
//
// Identity: entries without an InstanceIdentityEXT get a stable instance id
// derived from their handle, so temporal history and the TLAS follow the
// object across frames. They are classified static; a placement change demotes
// the entry to dynamic until it has not moved for retainedSettleFrames replays.
//
// Persistent static set (setPersistentStatic): a replay leaves the records of
// static entries that staticTraceEligible accepts out of the draw list. The
// tracer keeps its own copy of them, told which handles changed (joined, left,
// or re-resolved). Changes made after a replay first write that replay's set
// into the draw list (freezeStatic), so a frame always traces what its replay
// drew; expandStatic does the same for consumers other than the tracer.
namespace remix {
  using namespace scene;
  using namespace scene::detail;

  namespace {
    constexpr uint64_t retainedIdSeed = 0x52657461696e6564ull; // "Retained"

    uint64_t retainedInstanceId(uint64_t handle) {
      const uint64_t id = hashBytes(&handle, sizeof(handle), retainedIdSeed);
      return id ? id : 1;
    }

    bool allFinite(const Matrix &m) {
      return std::all_of(m.begin(), m.end(), [](float v) { return std::isfinite(v); });
    }

    // DrawInstance validation (material override included); copies the borrowed
    // GPU instancing transforms so nothing refers to caller memory.
    void describeRetained(const remixapi_InstanceInfo &info, InstanceDescription &d, std::vector<Matrix> &locals) {
      describeInstance(info, d);
      if (!d.hidden) {
        if (d.instanced) {
          locals.resize(d.instanceCount);
          for (uint32_t i = 0; i < d.instanceCount; ++i)
            locals[i] = transform(d.instanceValues[i]);
        }
      }
      d.instanceValues = nullptr;
    }

    // Runtime identity for entries without a caller identity.
    void identify(RetainedInstance &e) {
      e.callerIdentity = e.description.hasIdentity;
      if (e.callerIdentity)
        return;
      e.description.instanceId = retainedInstanceId(e.handle);
      e.description.classification = e.moving ? REMIXAPI_INSTANCE_DYNAMIC_EXT : REMIXAPI_INSTANCE_STATIC_EXT;
      e.description.rasterVisible = true;
      e.description.traceIdentity = false;
    }
  } // namespace

  bool scene::staticTraceEligible(const std::vector<Draw> &records) {
    if (records.empty())
      return false;
    for (const auto &draw : records)
      if (draw.classification != REMIXAPI_INSTANCE_STATIC_EXT || !draw.traced() || draw.material->fps ||
          draw.bones || draw.mesh->updateStream)
        return false;
    return true;
  }

  // Rebuilds the records. report: throw what DrawInstance would (create and
  // update); otherwise the caller catches and the entry draws nothing.
  void NativeResources::resolveRetained(RetainedInstance &e, bool report) {
    ++e.revision;
    e.resolvedModGeneration = m_modGeneration;
    e.staticEligible = e.staticEmissive = false;
    e.records.clear();
    e.resolved.clear();
    e.mesh.reset();
    e.surfaceCount = 0;
    e.watch = false;
    e.placed = true;
    const auto &d = e.description;
    if (d.hidden)
      return;
    const auto mesh = m_meshes.find(d.mesh);
    if (mesh == m_meshes.end()) {
      e.watch = true;
      return;
    }
    bool complete = true;
    auto override = overrideMaterial(d);
    if (d.materialOverride && !override)
      complete = false;
    const uint32_t count = e.instanceCount();
    const size_t surfaces = mesh->second->surfaces.size();
    if (uint64_t(count) * surfaces > 65536)
      throw invalid; // DrawInstance could never submit it either.
    m_modelScratch.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      auto &model = m_modelScratch[i];
      model = d.instanced ? multiply(d.model, e.locals[i]) : d.model;
      if (!allFinite(model)) {
        if (report)
          throw invalid;
        e.placed = false;
      }
    }
    std::vector<Draw> records;
    records.reserve(size_t(count) * surfaces);
    std::vector<std::shared_ptr<Material>> resolved;
    complete &= buildDraws(d, mesh->second, m_modelScratch.data(), count, override, false, records, &resolved);
    if (override)
      resolved.push_back(std::move(override));
    std::sort(resolved.begin(), resolved.end());
    resolved.erase(std::unique(resolved.begin(), resolved.end()), resolved.end());
    e.records = std::move(records);
    e.resolved = std::move(resolved);
    e.mesh = mesh->second;
    e.surfaceCount = uint32_t(surfaces);
    e.watch = !complete;
    e.staticEligible = !e.callerIdentity && staticTraceEligible(e.records);
    e.staticEmissive = std::any_of(e.records.begin(), e.records.end(), [](const Draw &draw) {
      const auto &emission = draw.material->emission;
      return draw.traced() && (emission[0] > 0 || emission[1] > 0 || emission[2] > 0);
    });
    // Resolved to live objects only: not stale.
    e.freshRetire = m_retireGeneration;
  }

  bool NativeResources::retainedStale(const RetainedInstance &e) const {
    if (e.mesh && e.mesh->retired)
      return true;
    for (const auto &material : e.resolved)
      if (material->retired)
        return true;
    return false;
  }

  void NativeResources::setRetainedWatch(RetainedInstance &e, bool watch) {
    if (e.watch != watch) {
      e.watch = watch;
      watch ? ++m_retainedWatch : --m_retainedWatch;
    }
  }

  void NativeResources::placeRetained(RetainedInstance &e) {
    ++e.revision;
    const uint32_t count = e.instanceCount();
    const bool resolved = e.records.size() == size_t(count) * e.surfaceCount;
    e.placed = true;
    for (uint32_t i = 0; i < count; ++i) {
      const Matrix model = e.description.instanced ? multiply(e.description.model, e.locals[i]) : e.description.model;
      e.placed &= allFinite(model);
      if (resolved)
        for (uint32_t s = 0; s < e.surfaceCount; ++s)
          e.records[size_t(i) * e.surfaceCount + s].model = model;
    }
  }

  void NativeResources::classifyRetained(RetainedInstance &e, uint32_t classification) {
    ++e.revision;
    e.description.classification = classification;
    for (auto &record : e.records)
      record.classification = classification;
    e.staticEligible = !e.callerIdentity && staticTraceEligible(e.records);
    e.staticEmissive = std::any_of(e.records.begin(), e.records.end(), [](const Draw &draw) {
      const auto &emission = draw.material->emission;
      return draw.traced() && (emission[0] > 0 || emission[1] > 0 || emission[2] > 0);
    });
  }

  // A moved static object leaves the static TLAS and SHARC's static geometry
  // signature alone while it moves: dynamic until it settles.
  void NativeResources::moveRetained(RetainedInstance &e) {
    if (e.callerIdentity)
      return;
    e.lastMove = m_retainedSerial;
    if (!e.moving) {
      e.moving = true;
      classifyRetained(e, REMIXAPI_INSTANCE_DYNAMIC_EXT);
    }
  }

  uint64_t NativeResources::createRetained(const remixapi_InstanceInfo &info) {
    if (!info.mesh)
      throw invalid;
    RetainedInstance e;
    describeRetained(info, e.description, e.locals);
    e.handle = m_nextRetained + 1;
    identify(e);
    resolveRetained(e, true);
    ++m_nextRetained;
    m_retainedIndex.emplace(e.handle, uint32_t(m_retained.size()));
    m_retainedWatch += e.watch;
    m_retained.push_back(std::move(e));
    return m_nextRetained;
  }

  void NativeResources::updateRetained(uint64_t handle, const remixapi_InstanceInfo &info) {
    if (!handle || !info.mesh)
      throw invalid;
    RetainedInstance next;
    describeRetained(info, next.description, next.locals);
    next.handle = handle;
    const auto found = m_retainedIndex.find(handle);
    RetainedInstance *current = found != m_retainedIndex.end() ? &m_retained[found->second] : nullptr;
    if (current) {
      // A re-description keeps visibility (and the motion state).
      next.visible = current->visible;
      next.moving = current->moving;
      next.lastMove = current->lastMove;
      // It stays in the static set until the next replay says otherwise.
      next.revision = current->revision;
      next.member = current->member;
      next.memberRevision = current->memberRevision;
      next.logged = current->logged;
    }
    identify(next);
    // New placements for the same instances are motion; a changed instance
    // count is new membership (streaming), which static geometry absorbs.
    if (current && !next.callerIdentity && !current->callerIdentity &&
        current->instanceCount() == next.instanceCount() &&
        (current->description.model != next.description.model || current->locals != next.locals)) {
      next.lastMove = m_retainedSerial;
      next.moving = true;
      next.description.classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    }
    resolveRetained(next, true);
    if (current) {
      freezeStatic();
      m_retainedWatch -= current->watch;
      *current = std::move(next);
      m_retainedWatch += current->watch;
      return;
    }
    // An unknown handle registers a new instance under it. The counter moves
    // past it so AddSceneInstancesEXT never hands it out.
    m_nextRetained = std::max(m_nextRetained, handle);
    m_retainedIndex.emplace(handle, uint32_t(m_retained.size()));
    m_retainedWatch += next.watch;
    m_retained.push_back(std::move(next));
  }

  void NativeResources::updateRetained(const remixapi_SceneInstanceChangeEXT *updates, uint32_t count) {
    if (!count)
      return;
    if (!updates)
      throw invalid;
    // Validate the whole batch before applying any of it.
    for (uint32_t i = 0; i < count; ++i) {
      if (!updates[i].instance)
        throw invalid;
      if (updates[i].fields & REMIXAPI_SCENE_INSTANCE_TRANSFORM_EXT)
        for (const auto &row : updates[i].transform.matrix)
          finite(row, 4);
    }
    freezeStatic();
    for (uint32_t i = 0; i < count; ++i) {
      const auto &update = updates[i];
      const auto found = m_retainedIndex.find(update.instance);
      if (found == m_retainedIndex.end())
        continue; // Unknown handles are ignored (an instance removed meanwhile).
      auto &e = m_retained[found->second];
      if (update.fields & REMIXAPI_SCENE_INSTANCE_TRANSFORM_EXT) {
        const Matrix model = transform(update.transform);
        if (model != e.description.model) {
          e.description.model = model;
          placeRetained(e);
          moveRetained(e);
        }
      }
      if (update.fields & REMIXAPI_SCENE_INSTANCE_VISIBLE_EXT)
        e.visible = update.visible != 0;
    }
  }

  void NativeResources::destroyRetained(const uint64_t *handles, uint32_t count) {
    if (!count)
      return;
    if (!handles)
      throw invalid;
    freezeStatic();
    for (uint32_t i = 0; i < count; ++i) {
      const auto found = m_retainedIndex.find(handles[i]);
      if (found == m_retainedIndex.end())
        continue; // Unknown or already destroyed.
      const uint32_t index = found->second;
      m_retainedIndex.erase(found);
      m_retainedWatch -= m_retained[index].watch;
      if (m_retained[index].member && !m_retained[index].logged)
        m_staticChanges.push_back(handles[i]); // Leaves the static set.
      if (index + 1 != m_retained.size()) {
        m_retained[index] = std::move(m_retained.back());
        m_retainedIndex[m_retained[index].handle] = index;
      }
      m_retained.pop_back();
    }
  }

  void NativeResources::drawRetained(const float *sceneOrigin) {
    float origin[3] = {};
    if (sceneOrigin) {
      finite(sceneOrigin, 3);
      std::copy_n(sceneOrigin, 3, origin);
    }
    const bool rebase = origin[0] != 0 || origin[1] != 0 || origin[2] != 0;
    // A second replay in one frame: the first one's static set joins the draw
    // list, and this one proceeds as usual.
    freezeStatic();
    // Rebased replays move every record: they bypass the static set.
    const bool persistent = m_staticEnabled && !rebase;
    ++m_retainedSerial;
    RetainedStatistics stats;
    stats.registered = m_retained.size();
    const bool modsChanged = m_modGeneration != m_retainedModGeneration;
    const bool resourcesChanged = m_resourceGeneration != m_retainedResourceGeneration && m_retainedWatch;
    m_retainedModGeneration = m_modGeneration;
    m_retainedResourceGeneration = m_resourceGeneration;
    const size_t changes = m_staticChanges.size();
    for (size_t index = 0; index < m_retained.size(); ++index) {
      auto &e = m_retained[index];
      // What the entry resolved to can only have been retired if something was
      // since it was last found current.
      bool stale = false;
      if (e.visible && e.freshRetire != m_retireGeneration) {
        stale = retainedStale(e);
        if (!stale)
          e.freshRetire = m_retireGeneration;
      }
      struct Leave {
        NativeResources &resources;
        RetainedInstance &entry;
        bool enabled;
        ~Leave() {
          if (enabled)
            resources.leaveStatic(entry);
        }
      } leave{*this, e, m_staticEnabled};
      if (modsChanged || (resourcesChanged && e.watch) || stale) {
        const bool watch = e.watch;
        try {
          resolveRetained(e, false);
        } catch (...) {
          e.records.clear();
          e.resolved.clear();
          e.mesh.reset();
          e.watch = true;
        }
        const bool now = e.watch;
        e.watch = watch;
        setRetainedWatch(e, now);
        ++stats.resolved;
      }
      if (e.moving && m_retainedSerial - e.lastMove >= retainedSettleFrames) {
        e.moving = false;
        classifyRetained(e, REMIXAPI_INSTANCE_STATIC_EXT);
      }
      stats.moving += e.moving;
      if (!e.visible || !e.placed || e.records.empty())
        continue;
      if (e.description.halo) {
        // A halo source: into its halo group, never the scene or the static set.
        appendHalo(e.description.haloParameters, std::vector<Draw>(e.records), rebase ? origin : nullptr);
        continue;
      }
      if (e.description.maskSource) {
        appendMask(e.description.maskParameters, std::vector<Draw>(e.records), rebase ? origin : nullptr);
        continue;
      }
      if (pendingDrawCount() + e.records.size() > 65536) {
        ++stats.overCapacity; // DrawInstance's per-frame limit.
        continue;
      }
      if (e.description.traceIdentity && !m_frameIds.insert(e.description.instanceId))
        continue; // Caller identity already drawn this frame.
      noteCut(e.description, e.records);
      if (persistent && e.staticEligible) {
        leave.enabled = false;
        m_staticPlacement.emplace_back(m_draws.size(), uint32_t(index));
        joinStatic(e);
        m_staticDraws += e.records.size();
        m_staticEmissive += e.staticEmissive;
        ++stats.appended;
        stats.draws += e.records.size();
        ++stats.staticMembers;
        stats.staticDraws += e.records.size();
        continue;
      }
      const size_t first = m_draws.size();
      if (m_hiddenTextures.empty()) {
        m_draws.insert(m_draws.end(), e.records.begin(), e.records.end());
      } else {
        for (const auto &rec : e.records) {
          if (!isDrawHidden(rec))
            m_draws.push_back(rec);
        }
      }
      if (rebase)
        for (size_t i = first; i < m_draws.size(); ++i)
          for (int c = 0; c < 3; ++c)
            m_draws[i].model[12 + c] -= origin[c];
      ++stats.appended;
      stats.draws += e.records.size();
    }
    m_staticDrawn = persistent;
    stats.staticChanges = m_staticChanges.size() - changes;
    m_retainedStats = stats;
  }

  void NativeResources::logStatic(RetainedInstance &e) {
    if (!e.logged) {
      e.logged = true;
      m_staticChanges.push_back(e.handle);
    }
  }

  void NativeResources::joinStatic(RetainedInstance &e) {
    if (!e.member || e.memberRevision != e.revision)
      logStatic(e);
    e.member = true;
    e.memberRevision = e.revision;
  }

  void NativeResources::leaveStatic(RetainedInstance &e) {
    if (e.member) {
      logStatic(e);
      e.member = false;
    }
  }

  void NativeResources::setPersistentStatic(bool enabled) {
    if (enabled == m_staticEnabled)
      return;
    freezeStatic();
    m_staticEnabled = enabled;
    ++m_staticEpoch;
    m_staticChanges.clear();
    for (auto &e : m_retained)
      e.member = e.logged = false;
  }

  void NativeResources::takeStaticChanges(std::vector<uint64_t> &out) {
    out.clear();
    std::swap(out, m_staticChanges);
    for (const uint64_t handle : out)
      if (const auto found = m_retainedIndex.find(handle); found != m_retainedIndex.end())
        m_retained[found->second].logged = false;
    if (!m_replicaChanges.empty())
      m_replicaChanges.clear();
  }

  bool NativeResources::applyViewModel(float scale) {
    constexpr uint32_t world = REMIXAPI_CAMERA_TYPE_WORLD, viewModel = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
    if (!m_cameraValid[world] || !m_cameraValid[viewModel] ||
        std::none_of(m_draws.begin(), m_draws.end(), [](const scene::Draw &d) { return d.camera == viewModel; }))
      return false;
    const auto &worldProjection = m_projections[world];
    auto projection = m_projections[viewModel];
    // Perspective cameras only (w = view-space depth).
    if (std::abs(worldProjection[15]) > 1e-6f || std::abs(projection[15]) > 1e-6f)
      return false;
    // The view model's field of view with the world camera's depth range
    // (its own near and far planes would scale it: dxvk-remix's correction).
    projection[10] = worldProjection[10];
    projection[11] = worldProjection[11];
    projection[14] = worldProjection[14];
    // world position = inverse(world view) inverse(world projection) x
    // view-model projection x shrink x view-model view x position.
    // In double precision: the views' translations (the eye, far from the
    // origin) nearly cancel, and the view model is a few units from the eye,
    // where single precision's error of the cancelled terms moves it by
    // pixels from frame to frame as the camera moves (a jittering gun).
    using Double = std::array<double, 16>; // column-major, as scene::Matrix
    const auto widen = [](const scene::Matrix &m) {
      Double d;
      std::copy(m.begin(), m.end(), d.begin());
      return d;
    };
    const auto product = [](const Double &a, const Double &b) {
      Double out{};
      for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
          double sum = 0;
          for (int k = 0; k < 4; ++k)
            sum += a[k * 4 + r] * b[c * 4 + k];
          out[c * 4 + r] = sum;
        }
      return out;
    };
    const auto invert = [](const Double &m, Double &out) { // Gauss-Jordan, partial pivoting
      double a[4][8];
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
          a[r][c] = m[c * 4 + r];
          a[r][c + 4] = r == c;
        }
      for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
          if (std::abs(a[r][c]) > std::abs(a[pivot][c]))
            pivot = r;
        if (std::abs(a[pivot][c]) < 1e-300)
          return false;
        std::swap(a[c], a[pivot]);
        const double inverse = 1 / a[c][c];
        for (double &v : a[c])
          v *= inverse;
        for (int r = 0; r < 4; ++r)
          if (r != c) {
            const double f = a[r][c];
            for (int k = 0; k < 8; ++k)
              a[r][k] -= f * a[c][k];
          }
      }
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
          out[c * 4 + r] = a[r][c + 4];
      return true;
    };
    Double shrink{};
    shrink[0] = shrink[5] = shrink[10] = scale;
    shrink[15] = 1;
    Double inverseView, inverseProjection;
    if (!invert(widen(m_authoredViews[world]), inverseView) || !invert(widen(worldProjection), inverseProjection))
      return false;
    const auto correction = product(
        inverseView,
        product(inverseProjection, product(widen(projection), product(shrink, widen(m_authoredViews[viewModel])))));
    if (!std::all_of(correction.begin(), correction.end(), [](double v) { return std::isfinite(v); }))
      return false;
    for (auto &draw : m_draws)
      if (draw.camera == viewModel && !draw.viewModel) {
        const auto model = product(correction, widen(draw.model));
        std::copy(model.begin(), model.end(), draw.model.begin());
        draw.camera = world;
        draw.viewModel = true;
        // Traced now: its mask group cuts (a scope's body around the lens) and
        // its camera culling applies, which DrawInstance (noteCut) could not
        // know while it was a view-model-camera draw.
        if (draw.traced()) {
          if (draw.cutGroup)
            m_cutGroups |= 1u << (draw.cutGroup - 1);
          m_cameraCull |= draw.cameraCull;
        }
      }
    // Its rasterized parts (mask silhouettes, overlays such as a scope's
    // picture) and the frame's raster depth use the world camera, whose near
    // plane would clip what reaches nearer (an eyepiece when aiming): the
    // near plane moves in to the view model's own, shrunk, for this frame.
    // A D3D-style projection (clip z = p10 z + p14, w = z): near = -p14 / p10.
    auto &p = m_projections[world];
    const float vmNear = -m_projections[viewModel][14] / m_projections[viewModel][10];
    const float worldNear = -p[14] / p[10], nearPlane = vmNear * scale;
    if (p[10] > 0 && p[11] > 0 && m_projections[viewModel][10] > 0 && std::isfinite(nearPlane) && nearPlane > 0 &&
        nearPlane < worldNear) {
      if (std::abs(1 - p[10]) > 1e-6f) {
        const float farPlane = p[14] / (1 - p[10]); // finite far plane: kept
        p[10] = farPlane / (farPlane - nearPlane);
      }
      p[14] = -nearPlane * p[10];
      m_cameras[world] = scene::multiply(p, m_views[world]);
      for (int c = 0; c < 4; ++c) {
        double value = 0;
        for (int k = 0; k < 4; ++k)
          value += (double(p[k * 4 + 3]) - p[k * 4 + 2]) * m_views[world][c * 4 + k];
        m_reverseDepth[world][c] = float(value);
      }
    }
    return true;
  }

  void NativeResources::expandStatic() {
    if (!m_staticDrawn)
      return;
    std::vector<Draw> merged;
    merged.reserve(m_draws.size() + m_staticDraws);
    size_t next = 0;
    for (const auto [position, index] : m_staticPlacement) {
      merged.insert(merged.end(), std::make_move_iterator(m_draws.begin() + next),
                    std::make_move_iterator(m_draws.begin() + position));
      next = position;
      const auto &records = m_retained[index].records;
      if (m_hiddenTextures.empty()) {
        merged.insert(merged.end(), records.begin(), records.end());
      } else {
        for (const auto &r : records) {
          if (!isDrawHidden(r))
            merged.push_back(r);
        }
      }
    }
    merged.insert(merged.end(), std::make_move_iterator(m_draws.begin() + next),
                  std::make_move_iterator(m_draws.end()));
    m_draws = std::move(merged);
    m_staticDrawn = false;
    m_staticDraws = m_staticEmissive = 0;
    m_staticPlacement.clear();
  }
} // namespace remix
