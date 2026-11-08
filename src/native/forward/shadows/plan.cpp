#include "forward/shadows/plan.h"
#include "forward/bake/draw_key.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace remix::forward::shadows {
  static View viewParameters(const scene::SphereLight &light, float range, float sceneScale) {
    View result;
    result.id = light.id;
    std::copy_n(light.position, 3, result.position.begin());
    result.nearPlane = .001f / sceneScale;
    result.farPlane = std::max(range, result.nearPlane * 2);
    return result;
  }
  static void projectFaces(View &result) {
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
  }
  View lightView(const scene::SphereLight &light, float range, float sceneScale) {
    auto view = viewParameters(light, range, sceneScale);
    projectFaces(view);
    return view;
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
  Coverage emptyCoverage() {
    Coverage result;
    result.fill({1, 1, -1, -1});
    return result;
  }
  static void projectCoverage(Vec4 &coverage, const Bounds &bounds, const Matrix &matrix, float nearPlane) {
    Vec4 rectangle{1, 1, -1, -1};
    bool full = !bounds.valid;
    for (uint32_t corner = 0; corner < 8 && !full; ++corner) {
      Vec3 point;
      double magnitude = 1;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        point[axis] = corner & (1u << axis) ? bounds.maximum[axis] : bounds.minimum[axis];
        magnitude = std::max(magnitude, double(std::abs(point[axis])));
      }
      const auto component = [&](uint32_t row) {
        double clip = matrix[12 + row], roundoff = std::abs(clip);
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const double term = double(matrix[axis * 4 + row]) * point[axis];
          clip += term;
          roundoff += std::abs(term);
        }
        return std::pair{clip, roundoff};
      };
      // Frustum testing already handled Z. The rectangle only needs X/Y/W.
      const auto [w, wRoundoff] = component(3);
      if (!std::isfinite(w) || w <= nearPlane) { full = true; break; }
      // Cover float transform/subtraction error, including distant origins.
      const double margin = 32 * std::numeric_limits<float>::epsilon() * magnitude / w;
      for (uint32_t axis = 0; axis < 2; ++axis) {
        const auto [clip, roundoff] = component(axis);
        const double value = clip / w;
        if (!std::isfinite(value)) { full = true; break; }
        // Narrow orthographic projections can amplify cancellation at distant
        // origins. Also bound transform/divide error in projected coordinates.
        const double error = std::max(margin, 32 * std::numeric_limits<float>::epsilon() *
            (roundoff + std::abs(value) * wRoundoff) / w);
        rectangle[axis] = std::min(rectangle[axis], float(value - error));
        rectangle[axis + 2] = std::max(rectangle[axis + 2], float(value + error));
      }
    }
    if (full) rectangle = {-1, -1, 1, 1};
    for (uint32_t axis = 0; axis < 2; ++axis) {
      coverage[axis] = std::min(coverage[axis], rectangle[axis]);
      coverage[axis + 2] = std::max(coverage[axis + 2], rectangle[axis + 2]);
    }
  }
  void CoverageProjection::include(Vec4 &coverage, const Bounds &bounds) const {
    if (m_frustum.intersects(bounds)) projectCoverage(coverage, bounds, m_matrix, m_nearPlane);
  }
  void includeCoverage(Vec4 &coverage, const Bounds &bounds, const Matrix &matrix, float nearPlane) {
    if (intersectsClip(bounds, matrix)) projectCoverage(coverage, bounds, matrix, nearPlane);
  }
  void includeCoverage(Coverage &coverage, const Bounds &bounds, const View &view) {
    for (uint32_t face = 0; face < faces; ++face)
      includeCoverage(coverage[face], bounds, view.matrices[face], view.nearPlane);
  }
  Selection Planner::update(std::span<const scene::SphereLight> lights, Vec3 eye, uint32_t budget,
                            float cutoff, float distance, float sceneScale,
                            std::shared_ptr<const scene::BakeScene> source, double seconds, float fadeSeconds,
                            std::span<const uint8_t> fallbackLights, std::span<const LightRange> ranges) {
    if (!fallbackLights.empty() && fallbackLights.size() != lights.size())
      throw std::invalid_argument("Shadow fallback policy does not match submitted lights");
    if (!ranges.empty() && ranges.size() != lights.size())
      throw std::invalid_argument("Shadow ranges do not match submitted lights");
    if (source && !source->ready)
      source.reset();
    if (m_source != source) {
      m_source = std::move(source);
      m_drawIndex.clear();
      if (m_source)
        for (uint32_t i = 0; i < m_source->draws.size(); ++i)
          if (!m_source->receiverOnly(i)) m_drawIndex.emplace(bake::drawLookupKey(m_source->draws[i]), i);
      m_valid.fill(false);
      m_assigned.fill(false);
      m_strength.fill(0);
    }
    struct Candidate {
      uint64_t id;
      uint32_t index;
      float range, priority;
      bool fallback;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(lights.size());
    for (uint32_t i = 0; i < lights.size(); ++i) {
      const float range = ranges.empty() ? influenceRadius(lights[i], cutoff) : ranges[i].influence;
      if (!std::isfinite(range) || range <= 0)
        continue;
      Vec3 delta;
      for (uint32_t c = 0; c < 3; ++c)
        delta[c] = eye[c] - lights[i].position[c];
      candidates.push_back({lights[i].id, i, range, range * range / std::max(dot(delta, delta), 1e-6f),
                            !fallbackLights.empty() && fallbackLights[i] != 0});
    }
    const auto ranksBefore = [](const Candidate &a, const Candidate &b) {
      if (a.fallback != b.fallback)
        return !a.fallback;
      if (a.priority != b.priority)
        return a.priority > b.priority;
      return a.id != b.id ? a.id < b.id : a.index < b.index;
    };
    Selection result;
    result.slots.resize(lights.size(), UINT32_MAX);
    budget = std::min(budget, maximumLights);
    result.overflow = uint32_t(candidates.size()) - std::min(uint32_t(candidates.size()), budget);
    const size_t desiredCount = std::min(size_t(budget), candidates.size());
    auto desiredEnd = candidates.begin() + desiredCount;
    std::partial_sort(candidates.begin(), desiredEnd, candidates.end(), ranksBefore);
    // The only other candidates that can get a map already own a slot and
    // may be fading out. Keep every occurrence of those IDs in ranked order.
    const auto retainedEnd = std::partition(desiredEnd, candidates.end(), [&](const Candidate &candidate) {
      for (uint32_t slot = 0; slot < budget; ++slot)
        if (m_assigned[slot] && m_views[slot].id == candidate.id) return true;
      return false;
    });
    std::sort(desiredEnd, retainedEnd, ranksBefore);
    candidates.erase(retainedEnd, candidates.end());
    const auto desired = [&](uint64_t id) {
      return std::any_of(candidates.begin(), candidates.begin() + desiredCount,
                         [&](const Candidate &candidate) { return candidate.id == id; });
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
                                      [&](const Candidate &c) { return c.id == m_views[slot].id; });
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
      const auto id = candidates[i].id;
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
          if (m_assigned[existing] && !result.active[existing] && m_views[existing].id == candidate.id) {
            slot = existing;
            break;
          }
      if (slot == UINT32_MAX) continue;
      // Projection matrices are needed only by lights that own a map. Keep
      // the ranked candidates compact instead of sorting six matrices each.
      auto view = viewParameters(lights[candidate.index], std::min(candidate.range, distance / sceneScale), sceneScale);
      result.active[slot] = true;
      result.strength[slot] = m_strength[slot];
      result.dirty[slot] = !m_valid[slot] || !sameView(m_views[slot], view);
      if (result.dirty[slot]) {
        projectFaces(view);
        m_views[slot] = view;
        m_valid[slot] = true;
      }
      result.views[slot] = m_views[slot];
    }
    // Inactive cached maps remain reusable until their slots are overwritten.
    return result;
  }
  bool Planner::captured(const scene::Draw &draw) const {
    if (!m_source)
      return false;
    const auto [first, last] = m_drawIndex.equal_range(bake::drawLookupKey(draw));
    for (auto i = first; i != last; ++i)
      if (scene::matchesBakeDraw(*m_source, i->second, draw))
        return true;
    return false;
  }
} // namespace remix::forward::shadows
