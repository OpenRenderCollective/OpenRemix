#include "scene/resources.h"
#include "scene/images.h"
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

  NativeResources::NativeResources() { reset(); }

  void NativeResources::reset() {
    m_draws.clear();
    m_frameIds.clear();
    m_hiddenTextures.clear();
    m_uiDraws.clear();
    m_overlay.clear();
    m_halos.clear();
    m_haloDraws = 0;
    m_masks.clear();
    m_maskDraws = 0;
    m_cutGroups = 0;
    m_cameraCull = 0;
    // Handles keep counting up: one issued before a restart never names a new instance.
    m_retained.clear();
    m_retainedIndex.clear();
    m_retainedWatch = 0;
    m_retainedStats = {};
    m_staticDrawn = false;
    m_staticDraws = m_staticEmissive = 0;
    m_staticPlacement.clear();
    m_staticChanges.clear();
    ++m_staticEpoch;
    m_materials.clear();
    m_modMaterials.clear();
    m_meshes.clear();
    m_textures.clear();
    m_uiTextures.clear();
    m_paths.clear();
    m_white.reset();
    m_default.reset();
    m_cameraValid = {};
    m_temporalEpoch = {};
    m_skyRadiance = {};
    m_cameraMedium = {};
    m_lights.clear();
    m_iesProfiles.reset();
    m_iesIndex.clear();
    m_cookieTextures.clear();
    m_light = {};
    m_dome = {};
    m_frameLights.clear();
    m_publishedLights.clear();
    m_lightsPublished = false;
    m_replica = false;
    m_replicaChanges.clear();
    for (auto &view : m_views)
      view = identity();
    for (auto &projection : m_projections)
      projection = identity();
    for (auto &camera : m_cameras) {
      camera = identity();
    }
    for (auto &depth : m_reverseDepth)
      depth = {0, 0, -1, 1};
    m_authoredViews = m_views;
    m_authoredCameras = m_cameras;
    m_authoredReverseDepth = m_reverseDepth;
    m_freeCamActive = false;
    std::fill(std::begin(m_tint), std::end(m_tint), 0.0f);
  }

  void NativeResources::finishFrame() {
    m_draws.clear();
    m_staticDrawn = false;
    m_staticDraws = m_staticEmissive = 0;
    m_staticPlacement.clear();
    m_frameIds.clear();
    m_uiDraws.clear();
    m_halos.clear();
    m_haloDraws = 0;
    m_masks.clear();
    m_maskDraws = 0;
    m_cutGroups = 0;
    m_cameraCull = 0;
    m_light = {};
    m_dome = {};
    m_frameLights.clear();
    m_publishedLights.clear();
    m_lightsPublished = false;
  }

  void NativeResources::setTextureHidden(uint64_t hash, bool hidden) {
    if (!hash)
      return;
    if (hidden)
      m_hiddenTextures.insert(hash);
    else
      m_hiddenTextures.erase(hash);
    restartStatic();
  }

  void NativeResources::clearHiddenTextures() {
    if (!m_hiddenTextures.empty()) {
      m_hiddenTextures.clear();
      restartStatic();
    }
  }

  void NativeResources::syncHiddenTextures(const std::vector<uint64_t> &hashes) {
    std::unordered_set<uint64_t> next(hashes.begin(), hashes.end());
    if (next != m_hiddenTextures) {
      m_hiddenTextures = std::move(next);
      restartStatic();
    }
  }
} // namespace remix
