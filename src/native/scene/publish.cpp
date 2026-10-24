#include "scene/resources.h"

#include <algorithm>

// Asynchronous presentation: the API thread publishes each frame into a second
// NativeResources that the render thread reads (NativeResources::publishFrame).
namespace remix {
  using namespace scene;

  void NativeResources::publishFrame(NativeResources &frame) {
    // The frame's draw lists move; state that persists across frames here is
    // copied. finishFrame below leaves this side as a finished frame would.
    frame.m_draws = std::move(m_draws);
    frame.m_hiddenTextures = m_hiddenTextures;
    if (!frame.m_hiddenTextures.empty()) {
      for (auto &d : frame.m_draws)
        frame.applyDrawHidden(d);
    }
    frame.m_uiDraws = std::move(m_uiDraws);
    frame.m_halos = std::move(m_halos);
    frame.m_haloDraws = m_haloDraws;
    frame.m_masks = std::move(m_masks);
    frame.m_maskDraws = m_maskDraws;
    frame.m_cutGroups = m_cutGroups;
    frame.m_cameraCull = m_cameraCull;
    frame.m_overlay = m_overlay;
    std::copy(std::begin(m_tint), std::end(m_tint), frame.m_tint);
    frame.m_cameras = m_cameras;
    frame.m_views = m_views;
    frame.m_authoredViews = m_authoredViews; // the host's own (applyViewModel ignores the free camera)
    frame.m_projections = m_projections;
    frame.m_reverseDepth = m_reverseDepth;
    frame.m_cameraValid = m_cameraValid;
    frame.m_temporalEpoch = m_temporalEpoch;
    frame.m_skyRadiance = m_skyRadiance;
    frame.m_cameraMedium = m_cameraMedium;
    frame.m_light = m_light;
    frame.m_iesProfiles = m_iesProfiles; // shared, immutable
    frame.m_cookieTextures = m_cookieTextures;
    frame.m_dome = m_dome;
    frame.m_visibilityPartition = m_visibilityPartition; // shared, immutable
    // Lights resolve now, as the tracer would have at this Present: a light
    // destroyed afterwards still lights this frame, one created does not.
    frame.m_frameLights = m_frameLights;
    frame.m_publishedLights = sphereLights();
    frame.m_lightsPublished = true;
    // The frame's consumers share this side's default material and white
    // texture, so their GPU copies are the ones the draws already use.
    if (m_default)
      frame.m_default = m_default;
    if (m_white)
      frame.m_white = m_white;
    // Weak path cache: pruned by raster preparation when frames render here.
    for (auto it = m_paths.begin(); it != m_paths.end();)
      it = it->second.expired() ? m_paths.erase(it) : std::next(it);

    // The persistent static set. A consumer of this table would take the
    // changed handles and read their entries; the replica takes them instead,
    // so it holds exactly the members this table has, and lists the handles
    // for its own consumer.
    frame.m_staticEnabled = m_staticEnabled;
    std::vector<uint64_t> changes;
    takeStaticChanges(changes);
    if (!frame.m_replica || frame.m_staticEpoch != m_staticEpoch) {
      // A new epoch (or a new replica): every member, in table order. The
      // consumer sees the new epoch and rebuilds from forEachStatic.
      frame.m_retained.clear();
      frame.m_retainedIndex.clear();
      frame.m_staticChanges.clear();
      frame.m_replicaChanges.clear();
      for (const auto &entry : m_retained)
        if (entry.member)
          frame.replicate(entry);
      frame.m_staticEpoch = m_staticEpoch;
      frame.m_replica = true;
    } else {
      for (const uint64_t handle : changes) {
        const auto *entry = retained(handle);
        if (entry && entry->member)
          frame.replicate(*entry);
        else
          frame.unreplicate(handle);
      }
    }
    for (const uint64_t handle : changes)
      frame.logReplica(handle);
    // This frame's placements, as replica slots (cached in the entry: a
    // placement costs no lookup while the slot holds the same handle).
    frame.m_staticPlacement.clear();
    frame.m_staticPlacement.reserve(m_staticPlacement.size());
    for (const auto [position, index] : m_staticPlacement) {
      auto &entry = m_retained[index];
      uint32_t slot = entry.replicaSlot;
      if (slot >= frame.m_retained.size() || frame.m_retained[slot].handle != entry.handle) {
        auto found = frame.m_retainedIndex.find(entry.handle);
        if (found == frame.m_retainedIndex.end()) {
          // Every placed entry is a member whose records were replicated when
          // they last changed; stay correct regardless.
          frame.replicate(entry);
          frame.logReplica(entry.handle);
          found = frame.m_retainedIndex.find(entry.handle);
        }
        slot = entry.replicaSlot = found->second;
      }
      frame.m_staticPlacement.emplace_back(position, slot);
    }
    frame.m_staticDrawn = m_staticDrawn;
    frame.m_staticDraws = m_staticDraws;
    frame.m_staticEmissive = m_staticEmissive;
    finishFrame();
  }

  void NativeResources::replicate(const RetainedInstance &entry) {
    const auto found = m_retainedIndex.find(entry.handle);
    if (found == m_retainedIndex.end()) {
      m_retainedIndex.emplace(entry.handle, uint32_t(m_retained.size()));
      m_retained.push_back(entry);
      m_retained.back().logged = false;
      return;
    }
    auto &slot = m_retained[found->second];
    const bool logged = slot.logged;
    slot = entry;
    slot.logged = logged;
  }

  void NativeResources::unreplicate(uint64_t handle) {
    const auto found = m_retainedIndex.find(handle);
    if (found == m_retainedIndex.end())
      return;
    const uint32_t index = found->second;
    m_retainedIndex.erase(found);
    if (index + 1 != m_retained.size()) {
      m_retained[index] = std::move(m_retained.back());
      m_retainedIndex[m_retained[index].handle] = index;
    }
    m_retained.pop_back();
  }

  void NativeResources::logReplica(uint64_t handle) {
    if (m_replicaChanges.insert(handle).second)
      m_staticChanges.push_back(handle);
  }

  void NativeResources::restartStatic() {
    // Membership stays: a consumer of the new epoch rebuilds its copy from the
    // current members (a replay already drawn this frame included).
    ++m_staticEpoch;
    m_staticChanges.clear();
    for (auto &entry : m_retained)
      entry.logged = false;
  }
} // namespace remix
