#include "forward/shadows/plan.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace remix::forward::shadows {
  View lightView(const scene::SphereLight &light, float range, float sceneScale) {
    View result;
    result.id = light.id;
    std::copy_n(light.position, 3, result.position.begin());
    result.nearPlane = .001f / sceneScale;
    result.farPlane = std::max(range, result.nearPlane * 2);
    const Vec3 forward[] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const Vec3 right[] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    const Vec3 down[] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
    const float a = result.farPlane / (result.farPlane - result.nearPlane);
    for (uint32_t face = 0; face < faces; ++face) {
      auto &matrix = result.matrices[face];
      for (uint32_t c = 0; c < 3; ++c) {
        matrix[c * 4] = right[face][c];
        matrix[c * 4 + 1] = down[face][c];
        matrix[c * 4 + 2] = forward[face][c] * a;
        matrix[c * 4 + 3] = forward[face][c];
      }
      matrix[12] = -dot(right[face], result.position);
      matrix[13] = -dot(down[face], result.position);
      matrix[14] = -dot(forward[face], result.position) * a - result.nearPlane * a;
      matrix[15] = -dot(forward[face], result.position);
    }
    return result;
  }
  bool sameView(const View &a, const View &b) {
    return a.id == b.id && a.position == b.position && a.nearPlane == b.nearPlane && a.farPlane == b.farPlane;
  }
  bool intersects(const Bounds &bounds, const View &view) {
    if (!bounds.valid)
      return true;
    float distance2 = 0;
    for (uint32_t c = 0; c < 3; ++c) {
      const float d = view.position[c] - std::clamp(view.position[c], bounds.minimum[c], bounds.maximum[c]);
      distance2 += d * d;
    }
    return distance2 <= view.farPlane * view.farPlane;
  }
  Selection Planner::update(std::span<const scene::SphereLight> lights, Vec3 eye, uint32_t budget,
                            float cutoff, float distance, float sceneScale,
                            std::shared_ptr<const scene::BakeScene> source, double seconds, float fadeSeconds,
                            std::span<const uint8_t> fallbackLights) {
    if (!fallbackLights.empty() && fallbackLights.size() != lights.size())
      throw std::invalid_argument("Shadow fallback policy does not match submitted lights");
    if (source && !source->ready)
      source.reset();
    if (m_source != source) {
      m_source = std::move(source);
      m_instances.clear();
      if (m_source)
        for (uint32_t i = 0; i < m_source->draws.size(); ++i)
          m_instances.emplace(m_source->draws[i].instanceId, i);
      m_valid.fill(false);
      m_assigned.fill(false);
      m_strength.fill(0);
    }
    struct Candidate {
      uint32_t index;
      View view;
      float priority;
      bool fallback;
    };
    std::vector<Candidate> candidates;
    for (uint32_t i = 0; i < lights.size(); ++i) {
      const float range = influenceRadius(lights[i], cutoff);
      if (!std::isfinite(range) || range <= 0)
        continue;
      auto view = lightView(lights[i], std::min(range, distance / sceneScale), sceneScale);
      Vec3 delta;
      for (uint32_t c = 0; c < 3; ++c)
        delta[c] = eye[c] - view.position[c];
      candidates.push_back({i, view, range * range / std::max(dot(delta, delta), 1e-6f),
                            !fallbackLights.empty() && fallbackLights[i] != 0});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
      if (a.fallback != b.fallback)
        return !a.fallback;
      if (a.priority != b.priority)
        return a.priority > b.priority;
      return a.view.id < b.view.id;
    });
    Selection result;
    result.slots.resize(lights.size(), UINT32_MAX);
    budget = std::min(budget, maximumLights);
    result.overflow = uint32_t(candidates.size()) - std::min(uint32_t(candidates.size()), budget);
    const size_t desiredCount = std::min(size_t(budget), candidates.size());
    const auto desired = [&](uint64_t id) {
      return std::any_of(candidates.begin(), candidates.begin() + desiredCount,
                         [&](const Candidate &candidate) { return candidate.view.id == id; });
    };
    const bool fading = std::isfinite(fadeSeconds) && fadeSeconds > 0;
    double elapsed = 0;
    if (std::isfinite(seconds)) {
      if (m_clockStarted) elapsed = std::max(0., seconds - m_seconds);
      m_seconds = m_clockStarted ? std::max(m_seconds, seconds) : seconds;
      m_clockStarted = true;
    }
    const float step = fading ? float(std::min(1., elapsed / fadeSeconds)) : 1.f;
    // Keep outgoing maps assigned until their shadow has faded completely.
    // Reusing a slot earlier would fade the new light with the old light's map.
    for (uint32_t slot = 0; slot < maximumLights; ++slot) {
      if (slot >= budget) {
        m_assigned[slot] = false;
        m_strength[slot] = 0;
      }
      if (!m_assigned[slot]) continue;
      const auto present = std::find_if(candidates.begin(), candidates.end(),
                                      [&](const Candidate &c) { return c.view.id == m_views[slot].id; });
      if (present == candidates.end()) {
        m_assigned[slot] = false;
        m_strength[slot] = 0;
        continue;
      }
      const bool wanted = desired(m_views[slot].id);
      m_strength[slot] = std::clamp(m_strength[slot] + (wanted ? step : -step), 0.f, 1.f);
      if (!wanted && m_strength[slot] == 0) m_assigned[slot] = false;
    }
    for (size_t i = 0; i < desiredCount; ++i) {
      const auto id = candidates[i].view.id;
      bool retained = false;
      for (uint32_t slot = 0; slot < budget; ++slot)
        retained |= m_assigned[slot] && m_views[slot].id == id;
      if (retained) continue;
      uint32_t free = budget;
      for (uint32_t slot = 0; slot < budget; ++slot)
        if (!m_assigned[slot] && m_valid[slot] && m_views[slot].id == id) {
          free = slot;
          break;
        }
      if (free == budget)
        free = uint32_t(std::find(m_assigned.begin(), m_assigned.begin() + budget, false) - m_assigned.begin());
      if (free == budget) continue; // outgoing fades still own the budget
      m_assigned[free] = true;
      m_strength[free] = fading ? 0.f : 1.f;
      // Publish the assignment below before replacing its cached identity.
      result.slots[candidates[i].index] = free;
    }
    for (const auto &candidate : candidates) {
      auto &slot = result.slots[candidate.index];
      if (slot == UINT32_MAX)
        for (uint32_t existing = 0; existing < budget; ++existing)
          if (m_assigned[existing] && !result.active[existing] && m_views[existing].id == candidate.view.id) {
            slot = existing;
            break;
          }
      if (slot == UINT32_MAX) continue;
      result.active[slot] = true;
      result.strength[slot] = m_strength[slot];
      result.views[slot] = candidate.view;
      result.dirty[slot] = !m_valid[slot] || !sameView(m_views[slot], candidate.view);
      m_views[slot] = candidate.view;
      m_valid[slot] = true;
    }
    // Inactive cached maps remain reusable until their slots are overwritten.
    return result;
  }
  bool Planner::captured(const scene::Draw &draw) const {
    if (!m_source)
      return false;
    const auto [first, last] = m_instances.equal_range(draw.instanceId);
    for (auto i = first; i != last; ++i)
      if (scene::matchesBakeDraw(*m_source, i->second, draw))
        return true;
    return false;
  }
} // namespace remix::forward::shadows
