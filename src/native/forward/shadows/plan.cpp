#include "forward/shadows/plan.h"
#include <algorithm>
#include <cmath>
#include <limits>

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
                            std::shared_ptr<const scene::BakeScene> source) {
    if (source && !source->ready)
      source.reset();
    if (m_source != source) {
      m_source = std::move(source);
      m_instances.clear();
      if (m_source)
        for (uint32_t i = 0; i < m_source->draws.size(); ++i)
          m_instances.emplace(m_source->draws[i].instanceId, i);
      m_valid.fill(false);
    }
    struct Candidate {
      uint32_t index;
      View view;
      float priority;
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
      candidates.push_back({i, view, range * range / std::max(dot(delta, delta), 1e-6f)});
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
      if (a.priority != b.priority)
        return a.priority > b.priority;
      return a.view.id < b.view.id;
    });
    Selection result;
    result.slots.resize(lights.size(), UINT32_MAX);
    budget = std::min(budget, maximumLights);
    result.overflow = uint32_t(candidates.size()) - std::min(uint32_t(candidates.size()), budget);
    candidates.resize(std::min(size_t(budget), candidates.size()));
    // Reserve surviving slots before assigning new lights, avoiding needless
    // cache invalidation when a higher-priority newcomer appears.
    for (const auto &candidate : candidates)
      for (uint32_t slot = 0; slot < budget; ++slot)
        if (!result.active[slot] && m_valid[slot] && m_views[slot].id == candidate.view.id) {
          result.slots[candidate.index] = slot;
          result.active[slot] = true;
          break;
        }
    for (const auto &candidate : candidates) {
      auto &slot = result.slots[candidate.index];
      if (slot == UINT32_MAX) {
        slot = uint32_t(std::find(result.active.begin(), result.active.begin() + budget, false) -
                        result.active.begin());
        result.active[slot] = true;
      }
      result.views[slot] = candidate.view;
      result.dirty[slot] = !m_valid[slot] || !sameView(m_views[slot], candidate.view);
      m_views[slot] = candidate.view;
      m_valid[slot] = true;
    }
    // Retain inactive cached slots; their maps remain valid until overwritten.
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
