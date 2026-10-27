#pragma once
#include "scene/id_set.h"
#include "scene/light_visibility.h"
#include "scene/retained.h"
#include "scene/temporal.h"
#include "scene/types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <unordered_set>
#include <utility>
#include <memory>
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "scene/free_camera.h"
#include "platform/utf8_path.h"

namespace remix {

  // CPU state is serialized by the API mutex. Only the presenting thread (the
  // startup thread, or OpenRemix's render thread with asynchronous
  // presentation, which reads only a published frame) touches GPU state.
  class NativeResources {
  public:
    NativeResources();
    ~NativeResources() = default;
    NativeResources(const NativeResources &) = delete;
    NativeResources &operator=(const NativeResources &) = delete;
    void reset();
    // remixapi_SetVisibilityPartitionEXT (null: none).
    void setVisibilityPartition(std::shared_ptr<const scene::VisibilityPartition> partition) {
      m_visibilityPartition = std::move(partition);
    }
    const std::shared_ptr<const scene::VisibilityPartition> &visibilityPartition() const {
      return m_visibilityPartition;
    }
    void clearModMaterials() {
      m_modMaterials.clear();
      m_modMerged.clear();
      m_modCandidates.clear();
      m_modRequested.clear();
      m_modRequests.clear();
      ++m_modGeneration;
    }
    // Lazily created mod materials: draws whose material hash is a candidate
    // are recorded (once) for the mod loader, which adds the replacement when
    // its textures are ready. Until then the original material is drawn.
    void setModMaterialCandidates(std::unordered_set<uint64_t> candidates) {
      clearModMaterials();
      m_modCandidates = std::move(candidates);
    }
    void addModMaterial(uint64_t hash, remixapi_MaterialHandle handle) {
      if (!handle)
        return;
      auto it = m_materials.find(uint64_t(reinterpret_cast<uintptr_t>(handle)));
      if (it != m_materials.end()) {
        m_modMaterials[hash] = it->second;
        m_modMerged.clear();
        ++m_modGeneration;
      }
    }
    std::vector<uint64_t> takeModMaterialRequests() { return std::exchange(m_modRequests, {}); }
    void replaceModMaterials(const std::map<uint64_t, remixapi_MaterialHandle> &handles) {
      std::unordered_map<uint64_t, std::shared_ptr<scene::Material>> next;
      for (auto [hash, handle] : handles) {
        if (handle) {
          auto it = m_materials.find(uint64_t(reinterpret_cast<uintptr_t>(handle)));
          if (it != m_materials.end())
            next[hash] = it->second;
        }
      }
      m_modMaterials = std::move(next);
      m_modMerged.clear();
      ++m_modGeneration;
    }
    void invalidateTexturePaths() { m_paths.clear(); }
    void cacheTexturePath(const std::string &path, const std::shared_ptr<scene::Texture> &texture) {
      m_paths[std::filesystem::absolute(utf8Path(path)).lexically_normal()] = texture;
    }
    void createMaterial(const remixapi_MaterialInfo &info, remixapi_MaterialHandle &handle);
    void setMaterialSourceHash(remixapi_MaterialHandle handle, uint64_t hash) {
      auto it = m_materials.find(uint64_t(reinterpret_cast<uintptr_t>(handle)));
      if (it != m_materials.end()) {
        it->second->sourceHash = hash;
        ++m_modGeneration;
      }
    }
    void destroyMaterial(remixapi_MaterialHandle handle);
    void createMesh(const remixapi_MeshInfo &info, remixapi_MeshHandle &handle);
    void destroyMesh(remixapi_MeshHandle handle);
    void createTexture(const remixapi_TextureInfo &info, remixapi_TextureHandle &handle);
    // Stores pixels prepared outside the API mutex (see scene::compressTexture).
    void createTexture(const remixapi_TextureInfo &info, scene::Pixels &&pixels, remixapi_TextureHandle &handle);
    // remixapi_UpdateTextureEXT: rewrites a region of a dynamic texture.
    void updateTexture(remixapi_TextureHandle handle, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                       remixapi_Format format, const void *pixels, uint32_t rowPitch);
    void destroyTexture(remixapi_TextureHandle handle);
    const std::unordered_map<uint64_t, std::shared_ptr<scene::Texture>> &textures() const { return m_textures; }
    const std::unordered_map<std::filesystem::path, std::weak_ptr<scene::Texture>> &paths() const { return m_paths; }
    const std::unordered_set<uint64_t> &hiddenTextures() const { return m_hiddenTextures; }
    bool isTextureHidden(uint64_t hash) const { return hash && m_hiddenTextures.find(hash) != m_hiddenTextures.end(); }
    bool isDrawHidden(const scene::Draw &draw) const {
      if (m_hiddenTextures.empty() || !draw.material)
        return false;
      if (draw.material->texture && isTextureHidden(draw.material->texture->getHash()))
        return true;
      if (draw.material->sourceHash && isTextureHidden(draw.material->sourceHash))
        return true;
      return false;
    }
    void applyDrawHidden(scene::Draw &draw) const {
      if (isDrawHidden(draw)) {
        draw.rasterVisible = false;
        draw.alphaOverride = true;
        draw.compare = 0; // VK_COMPARE_OP_NEVER: ray tracing any-hit discards the hit
        draw.ignoreAlpha = false;
      }
    }
    void setTextureHidden(uint64_t hash, bool hidden);
    void clearHiddenTextures();
    void syncHiddenTextures(const std::vector<uint64_t> &hashes);
    // "Keep BC textures compressed": applies to textures created afterwards.
    // Atomic so the API layer can choose the preparation path before locking.
    void setKeepCompressedTextures(bool value) { m_keepCompressed.store(value, std::memory_order_relaxed); }
    bool keepCompressedTextures() const { return m_keepCompressed.load(std::memory_order_relaxed); }
    // Honour remixapi_MeshInfoDeformEXT ("Refresh mesh geometry in
    // place"). Off: the struct is ignored and a repeated CreateMesh replaces the
    // mesh as before.
    void setRefreshGeometry(bool value) { m_refreshGeometry = value; }
    bool refreshGeometry() const { return m_refreshGeometry; }
    void setupCamera(const remixapi_CameraInfo &info);
    void applyFreeCamera(const scene::FreeCameraState &freeCam);
    void drawInstance(const remixapi_InstanceInfo &info, remixapi_MaterialHandle materialOverride = nullptr);
    // Scene instances (remixapi_AddSceneInstancesEXT). Handles are nonzero and never reused.
    uint64_t createRetained(const remixapi_InstanceInfo &info);
    void updateRetained(uint64_t handle, const remixapi_InstanceInfo &info);
    void updateRetained(const remixapi_SceneInstanceChangeEXT *changes, uint32_t count);
    void destroyRetained(const uint64_t *handles, uint32_t count);
    void drawRetained(const float *sceneOrigin);
    size_t retainedCount() const { return m_retained.size(); }
    const scene::RetainedStatistics &retainedStatistics() const { return m_retainedStats; }
    // DrawRetainedInstances calls after which a moved instance returns to static.
    static constexpr uint64_t retainedSettleFrames = 240;
    // Persistent static set ("rtx.native.scene.persistentStatic"). While on,
    // drawRetained keeps retained instances that staticTraceEligible accepts
    // out of the draw list: they are members of the static set, which a
    // consumer (the path tracer) keeps its own copy of, updated from the
    // handles whose membership or records changed (takeStaticChanges). Takes
    // effect at the next drawRetained; switching bumps staticEpoch.
    void setPersistentStatic(bool enabled);
    bool persistentStatic() const { return m_staticEnabled; }
    // This frame's members are drawn through the static set, not m_draws.
    // Cleared by finishFrame and by expandStatic.
    bool staticDrawn() const { return m_staticDrawn; }
    // Changes when a consumer must rebuild its copy from every member.
    uint64_t staticEpoch() const { return m_staticEpoch; }
    // Draws of this frame's static set (not in draws()), and its members with
    // a traced emissive record.
    size_t staticDrawCount() const { return m_staticDraws; }
    size_t staticEmissiveCount() const { return m_staticEmissive; }
    // Handles whose membership or records changed since the last call.
    void takeStaticChanges(std::vector<uint64_t> &out);
    // Current members (a consumer's full rebuild), in retained table order.
    template <class F> void forEachStatic(F &&f) const {
      for (const auto &entry : m_retained)
        if (entry.member)
          f(entry);
    }
    const scene::RetainedInstance *retained(uint64_t handle) const {
      const auto found = m_retainedIndex.find(handle);
      return found == m_retainedIndex.end() ? nullptr : &m_retained[found->second];
    }
    // Every scene instance, in table order (usd/capture.cpp).
    const std::vector<scene::RetainedInstance> &retainedInstances() const { return m_retained; }
    // Writes this frame's static set into the draw list exactly where replay
    // without the set would have put it, for consumers other than the tracer
    // (raster rendering), and clears staticDrawn.
    void expandStatic();
    // Path-traced view models: this frame's VIEW_MODEL-camera draws move into
    // the world camera's view, placed so that it projects them where their own
    // camera would (its field of view; the world camera's depth range), shrunk
    // toward the eye by `scale` (the same image, nearer), and become world
    // draws marked viewModel (traced, or raster-only overlays depth-tested
    // against the traced frame). Uses the host's cameras (not the free
    // camera). False: no valid perspective world or view-model camera; the
    // draws stay as they were.
    bool applyViewModel(float scale);
    // Asynchronous presentation ("rtx.native.asyncPresent"). publishFrame
    // hands this frame to `frame`, a second NativeResources that a render
    // thread reads while API calls keep changing this one, and ends the frame
    // here (finishFrame). Draw lists move; cameras, lights, the overlay and
    // the frame's sphere lights are copied. Meshes, materials and textures are
    // shared, never copied: the published draws keep them alive, so creating
    // or destroying resources here never reaches the render thread.
    //
    // `frame` keeps a replica of the persistent static set: the members, as
    // this table has them, updated from the handles whose membership or
    // records changed (takeStaticChanges here), with those handles in its own
    // change list. Its staticDrawn, staticEpoch, takeStaticChanges,
    // forEachStatic, retained and expandStatic answer exactly as this table's
    // would have at publication, so the tracer's copy (StaticScene) syncs from
    // it unchanged. Only `frame`'s owner may read `frame` afterwards; nothing
    // here keeps a reference to it.
    void publishFrame(NativeResources &frame);
    // The persistent static setting as set after publication (persistentStatic
    // of the published frame), as the frame's consumers read it.
    void publishStaticSetting(NativeResources &frame) const { frame.m_staticEnabled = m_staticEnabled; }
    // A consumer switches between this table and a published replica: the
    // static set starts a new epoch, so its copy is rebuilt from the members.
    void restartStatic();
    // Set while frames are rendered on another thread: shared resource state
    // that rendering writes (Material::emissionCache) is not read from here.
    void setConcurrentConsumer(bool value) { m_concurrentConsumer = value; }
    void registerUiTexture(uint64_t id, uint32_t width, uint32_t height, remixapi_Format format,
                           const void *pixels, uint64_t size);
    void updateUiTextureRegion(uint64_t id, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                               remixapi_Format format, const void *pixels, uint32_t rowPitch);
    void freeUiTexture(uint64_t id);
    void submitUi(const remixapi_UIDrawList *info);
    void overlay(const void *pixels, uint32_t width, uint32_t height, remixapi_Format format, float opacity);
    void setTint(float r, float g, float b, float a);
    void finishFrame();
    // Surface draws of this frame, including a static set drawn separately.
    size_t pendingDrawCount() const { return m_draws.size() + m_staticDraws; }

    const std::vector<scene::Draw> &draws() const { return m_draws; }
    // Halo sources of this frame (remixapi_InstanceInfoHaloEXT), in submission
    // order of each group's first instance. Not part of draws().
    const std::vector<scene::HaloGroup> &halos() const { return m_halos; }
    // Per-frame limits; instances beyond them are ignored.
    static constexpr size_t maxHaloGroups = 32, maxHaloDraws = 4096;
    // Mask sources of this frame (remixapi_InstanceInfoMaskEXT, role SOURCE),
    // in the order their coverage is drawn. Not part of draws().
    const std::vector<scene::MaskRun> &masks() const { return m_masks; }
    static constexpr size_t maxMaskRuns = 64, maxMaskDraws = 4096;
    // Groups (bit group - 1) that this frame's traceable scene instances with
    // role CUT name, wherever those instances are (draws() or the static set).
    uint32_t cutGroups() const { return m_cutGroups; }
    // remixapi_InstanceInfoCameraCullingEXT flags that this frame's traceable
    // scene instances carry (their union), wherever those instances are.
    uint32_t cameraCull() const { return m_cameraCull; }
    const scene::Matrix &camera(uint32_t type = 0) const { return m_cameras[type]; }
    const scene::Matrix &view(uint32_t type = 0) const { return m_views[type]; }
    const scene::Matrix &projection(uint32_t type = 0) const { return m_projections[type]; }
    const std::array<float, 4> &reverseDepth(uint32_t type = 0) const { return m_reverseDepth[type]; }
    bool cameraValid() const { return m_cameraValid[0]; }
    scene::TemporalEpoch temporalEpoch() const { return m_temporalEpoch; }
    const std::array<float, 3> &skyRadiance() const { return m_skyRadiance; }
    // remixapi_CameraMediumEXT: transmittance colour, measurement distance (0: in air).
    const std::array<float, 4> &cameraMedium() const { return m_cameraMedium; }
    void createLight(const remixapi_LightInfo &info, remixapi_LightHandle &handle);
    // IES profiles (remixapi_LightInfoIesProfileEXT), scene::kIesBins floats
    // each. Adding one makes a new vector, so a published frame keeps its own.
    std::shared_ptr<const std::vector<float>> iesProfiles() const { return m_iesProfiles; }
    // Light cookies' textures (remixapi_LightInfoCookieEXT), by slot: a
    // sphere light's cookie is its slot + 1. Slots are per texture and kept.
    const std::vector<std::shared_ptr<scene::Texture>> &cookieTextures() const { return m_cookieTextures; }
    std::shared_ptr<scene::Material> cookieMaterial() {
      if (!m_cookieMaterial) {
        m_cookieMaterial = std::make_shared<scene::Material>(*defaultMaterial());
        m_cookieMaterial->filter = 1;
        m_cookieMaterial->wrapU = m_cookieMaterial->wrapV = 0;
      }
      return m_cookieMaterial;
    }
    void updateLight(remixapi_LightHandle handle, const remixapi_LightInfo &info);
    void destroyLight(remixapi_LightHandle handle);
    void drawLight(remixapi_LightHandle handle);
    remixapi_LightHandle overrideLight(remixapi_LightHandle original, uint64_t hash,
                                       const remixapi_LightInfoUSDEXT &overrides, bool transform,
                                       remixapi_Path texture = nullptr);
    std::shared_ptr<scene::Material> defaultMaterial() {
      if (!m_default) {
        m_default = std::make_shared<scene::Material>();
        m_default->texture = texturePath(nullptr);
      }
      return m_default;
    }
    // Sampler state for a dome light's equirect map: linear, wrapping in
    // longitude, clamped at the poles. Kept alive: GPU material state is
    // cached per material object.
    std::shared_ptr<scene::Material> domeMaterial() {
      if (!m_domeMaterial) {
        m_domeMaterial = std::make_shared<scene::Material>(*defaultMaterial());
        m_domeMaterial->filter = 1;
        m_domeMaterial->wrapV = 0;
      }
      return m_domeMaterial;
    }
    const scene::DomeLight &dome() const { return m_dome; }
    const scene::DistantLight &light() const { return m_light; }
    std::vector<scene::SphereLight> sphereLights() const;
    bool hasSphereLights() const { return !m_frameLights.empty(); }

    // A light as created: its USD-style parameters (overrideLight merges
    // into them; usd/capture.cpp writes them out).
    struct LightSource {
      remixapi_StructType type{};
      remixapi_Transform transform{};
      remixapi_Float3D color{1, 1, 1};
      remixapi_Float3D direction{0, 0, -1};
      float intensity = 1, exposure = 0, temperature = 6500, radius = .5f, width = 1, height = 1, length = 1;
      float angle = .009250245f, cone = 3.14159265f, softness = 0, focus = 0;
      remixapi_Bool temperatureEnabled = false;
      std::filesystem::path texture;
      // remixapi_LightInfoIesProfileEXT's text (empty: none), and
      // remixapi_LightInfoCookieEXT's texture (as named, and what it resolved
      // to; null: none), up and field of view.
      std::string ies;
      std::filesystem::path cookiePath;
      std::shared_ptr<scene::Texture> cookie;
      remixapi_Float3D cookieUp{0, 0, 1};
      float cookieFov = 0;
    };
    // USD capture (usd/capture.cpp): what a handle's hash resolves to now
    // (null: none), the host's own view (not the free camera's), and whether
    // a texture is the white one materials without an albedo map get.
    std::shared_ptr<const scene::Mesh> findMesh(uint64_t hash) const {
      const auto found = m_meshes.find(hash);
      return found == m_meshes.end() ? nullptr : found->second;
    }
    std::shared_ptr<const scene::Material> findMaterial(uint64_t hash) const {
      const auto found = m_materials.find(hash);
      return found == m_materials.end() ? nullptr : found->second;
    }
    const LightSource *lightSource(uint64_t hash) const {
      const auto found = m_lights.find(hash);
      return found == m_lights.end() ? nullptr : &found->second.source;
    }
    const scene::Matrix &authoredView(uint32_t type = 0) const { return m_authoredViews[type]; }
    bool isDefaultTexture(const scene::Texture *texture) const {
      return texture && m_default && texture == m_default->texture.get();
    }

  protected:
    std::shared_ptr<scene::Texture> texturePath(remixapi_Path path);
    std::shared_ptr<scene::Texture> dataTexturePath(remixapi_Path path);
    void storeTexture(uint64_t hash, scene::Pixels &&pixels, remixapi_TextureHandle &handle);
    std::atomic<bool> m_keepCompressed{false};
    bool m_refreshGeometry = false;
    std::shared_ptr<scene::Texture> m_white;
    std::shared_ptr<scene::Material> m_default, m_domeMaterial, m_cookieMaterial;
    std::shared_ptr<const scene::VisibilityPartition> m_visibilityPartition;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Material>> m_materials, m_modMaterials;
    // A mod material given the albedo of the material it replaces
    // (instances.cpp), by that material's hash; remade when either changes.
    struct ModMerged {
      std::weak_ptr<scene::Material> mod, original;
      std::shared_ptr<scene::Material> merged;
    };
    std::unordered_map<uint64_t, ModMerged> m_modMerged;
    std::unordered_set<uint64_t> m_modCandidates, m_modRequested;
    std::vector<uint64_t> m_modRequests;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Mesh>> m_meshes;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Texture>> m_textures, m_uiTextures;
    std::unordered_set<uint64_t> m_hiddenTextures;
    std::unordered_map<std::filesystem::path, std::weak_ptr<scene::Texture>> m_paths;
    std::array<scene::Matrix, 3> m_cameras, m_views, m_projections;
    std::array<scene::Matrix, 3> m_authoredCameras, m_authoredViews;
    std::array<std::array<float, 4>, 3> m_reverseDepth, m_authoredReverseDepth;
    bool m_freeCamActive = false;
    struct LightDefinition {
      LightSource source;
      scene::DistantLight distant;
      std::vector<scene::SphereLight> emitters;
      scene::DomeLight dome;
    };
    std::unordered_map<uint64_t, LightDefinition> m_lights;
    // A profile's number (+ 1) by its text's hash; the profile for an IES text.
    std::shared_ptr<const std::vector<float>> m_iesProfiles;
    std::unordered_map<uint64_t, uint32_t> m_iesIndex;
    uint32_t iesProfile(std::string_view text);
    std::vector<std::shared_ptr<scene::Texture>> m_cookieTextures;
    std::unordered_set<uint64_t> m_frameLights;
    // A published frame's sphere lights, as sphereLights() returned them at
    // publication (m_frameLights then only tells whether there are any).
    std::vector<scene::SphereLight> m_publishedLights;
    bool m_lightsPublished = false;
    scene::DistantLight m_light;
    scene::DomeLight m_dome;
    std::array<bool, 3> m_cameraValid = {};
    scene::TemporalEpoch m_temporalEpoch;
    std::array<float, 3> m_skyRadiance = {};
    std::array<float, 4> m_cameraMedium = {};
    std::vector<scene::Draw> m_draws, m_uiDraws, m_overlay;
    std::vector<scene::HaloGroup> m_halos;
    size_t m_haloDraws = 0;
    // Moves a halo instance's draws into its group (the group with equal
    // parameters, or a new one); origin: a retained replay's scene origin.
    void appendHalo(const scene::Halo &halo, std::vector<scene::Draw> &&draws, const float *origin = nullptr);
    std::vector<scene::MaskRun> m_masks;
    size_t m_maskDraws = 0;
    uint32_t m_cutGroups = 0, m_cameraCull = 0;
    // Records the group of a scene instance with role CUT that can be traced
    // (a rasterized one needs no coverage ahead of the frame), and the camera
    // culling a traced instance asks for.
    void noteCut(const scene::InstanceDescription &description, const std::vector<scene::Draw> &draws);
    // Moves a mask source's draws into a run. It joins the latest run with
    // equal parameters unless a run after that one read this group's coverage
    // (its `within`) or wrote the coverage these sources are limited to: a
    // source sees `within` as accumulated by the sources submitted before it.
    void appendMask(const scene::Mask &mask, std::vector<scene::Draw> &&draws, const float *origin = nullptr);
    scene::IdSet m_frameIds;
    float m_tint[4] = {};

    // Appends one Draw per (instance, surface) of mesh, as DrawInstance builds
    // them. strict applies DrawInstance's rule that a surface material handle
    // must be live; otherwise such a surface uses the default material and false
    // is returned. resolved receives the materials the surface handles named.
    bool buildDraws(const scene::InstanceDescription &description, const std::shared_ptr<scene::Mesh> &mesh,
                    const scene::Matrix *models, uint32_t count,
                    const std::shared_ptr<scene::Material> &materialOverride, bool strict,
                    std::vector<scene::Draw> &out, std::vector<std::shared_ptr<scene::Material>> *resolved);
    // The live material an InstanceInfoMaterialEXT names; null keeps
    // the mesh's own materials (no override, or a dead handle).
    std::shared_ptr<scene::Material> overrideMaterial(const scene::InstanceDescription &description) const;
    void resolveRetained(scene::RetainedInstance &entry, bool strict);
    bool retainedStale(const scene::RetainedInstance &entry) const;
    void setRetainedWatch(scene::RetainedInstance &entry, bool watch);
    void placeRetained(scene::RetainedInstance &entry);
    void classifyRetained(scene::RetainedInstance &entry, uint32_t classification);
    void moveRetained(scene::RetainedInstance &entry);
    // Dense table; m_retainedIndex maps a handle to its slot.
    std::vector<scene::RetainedInstance> m_retained;
    std::unordered_map<uint64_t, uint32_t> m_retainedIndex;
    std::vector<scene::Matrix> m_modelScratch;
    scene::RetainedStatistics m_retainedStats;
    uint64_t m_nextRetained = 0, m_retainedSerial = 0;
    // Bumped when a mesh or material handle starts resolving to a new object,
    // and when the mod material mapping changes.
    uint64_t m_resourceGeneration = 0, m_modGeneration = 0;
    uint64_t m_retainedResourceGeneration = 0, m_retainedModGeneration = 0;
    uint32_t m_retainedWatch = 0;
    // Bumped whenever a mesh or material is marked retired: an entry verified
    // not stale at the current value cannot be stale.
    uint64_t m_retireGeneration = 0;
    // Persistent static set. m_staticPlacement: where each member drawn this
    // frame would sit in m_draws (position, table slot), for expandStatic.
    void joinStatic(scene::RetainedInstance &entry);
    void leaveStatic(scene::RetainedInstance &entry);
    void logStatic(scene::RetainedInstance &entry);
    // Before a retained change made after this frame's replay: the frame keeps
    // what the replay drew (in m_draws, as without the set).
    void freezeStatic() {
      if (m_staticDrawn)
        expandStatic();
    }
    bool m_staticEnabled = false, m_staticDrawn = false;
    uint64_t m_staticEpoch = 1;
    size_t m_staticDraws = 0, m_staticEmissive = 0;
    std::vector<std::pair<size_t, uint32_t>> m_staticPlacement;
    std::vector<uint64_t> m_staticChanges;
    // A published frame's replica of the static set (publishFrame): whether it
    // holds the members of m_staticEpoch, and the handles in m_staticChanges.
    void replicate(const scene::RetainedInstance &entry);
    void unreplicate(uint64_t handle);
    void logReplica(uint64_t handle);
    bool m_replica = false;
    std::unordered_set<uint64_t> m_replicaChanges;
    bool m_concurrentConsumer = false;
  };
} // namespace remix
