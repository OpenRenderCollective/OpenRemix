#include "forward/probes/evaluation.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace remix::forward::probes {
  namespace {
    constexpr uint32_t raysPerOrigin = 6 * ProjectionAudit::resolution * ProjectionAudit::resolution;
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Reflection projection evaluation superseded");
    }
  } // namespace
  ProjectionAudit::ProjectionAudit(std::vector<FittedRegion> regions, std::stop_token cancelled)
      : m_regions(std::move(regions)), m_statistics(m_regions.size()), m_cancelled(cancelled) {
    check(cancelled);
    if (m_regions.size() > maximumCandidates)
      throw std::invalid_argument("Reflection candidate budget exceeded");
    m_rays.reserve(std::min(m_regions.size(), size_t(maximumOrigins)) * raysPerOrigin);
    for (size_t i = 0; i < m_regions.size(); ++i) {
      check(cancelled);
      const auto &region = m_regions[i];
      // Keep per-region validation even when its origin reuses existing queries.
      const Capture capture{region.bounds, region.site.position, resolution, {}, {}};
      const auto points = probes::rays(capture);
      const auto origin = std::find_if(m_origins.begin(), m_origins.end(), [&](const auto &entry) {
        return entry.position == region.site.position;
      });
      if (origin != m_origins.end()) {
        origin->regions.push_back(i);
        continue;
      }
      if (m_origins.size() == maximumOrigins)
        throw std::invalid_argument("Reflection origin budget exceeded");
      m_origins.push_back({region.site.position, {i}});
      m_rays.insert(m_rays.end(), points.begin(), points.end());
    }
    m_seen.resize(m_rays.size());
  }
  bool ProjectionAudit::observe(uint32_t first, std::span<const bake::Result> results) {
    if (first > m_rays.size() || results.size() > m_rays.size() - first)
      return false;
    // Refuse the entire batch before updating state. Slots may complete out of
    // order, but duplicate or invalid readbacks must not bias the assessment.
    for (size_t i = 0; i < results.size(); ++i) {
      const auto &value = results[i];
      const float back = value.statistics[0], miss = value.statistics[1];
      if (m_seen[first + i] || !std::isfinite(value.irradiance[0][3]) || value.irradiance[0][3] <= 0 ||
          (back != 0 && back != 1) || (miss != 0 && miss != 1) || (back > 0 && miss > 0) ||
          value.statistics[2] != 1)
        return false;
    }
    for (size_t i = 0; i < results.size(); ++i) {
      const size_t index = first + i;
      const auto &direction = m_rays[index].normal;
      const double dominant =
          std::max({std::abs(direction[0]), std::abs(direction[1]), std::abs(direction[2])});
      const double weight = dominant * dominant * dominant; // cube-map solid-angle Jacobian
      const auto &value = results[i];
      // Alternative projection boxes share first-hit geometry at this origin.
      // Assess each box on the CPU without tracing identical directions again.
      for (const auto regionIndex : m_origins[index / raysPerOrigin].regions) {
        const auto &region = m_regions[regionIndex];
        float exit = std::numeric_limits<float>::max();
        uint32_t face = 0;
        for (uint32_t axis = 0; axis < 3; ++axis)
          if (std::abs(direction[axis]) > 1e-10f) {
            const bool positive = direction[axis] > 0;
            const float plane = positive ? region.bounds.maximum[axis] : region.bounds.minimum[axis];
            const float distance = (plane - region.site.position[axis]) / direction[axis];
            if (distance < exit) {
              exit = distance;
              face = axis * 2 + uint32_t(!positive);
            }
          }
        const bool missed = value.statistics[1] != 0;
        const double relative = (double(value.irradiance[0][3]) - exit) / std::max(double(exit), 1e-30);
        auto &statistics = m_statistics[regionIndex];
        if (!missed) {
          Vec3 hit;
          for (uint32_t axis = 0; axis < 3; ++axis)
            hit[axis] = region.site.position[axis] + direction[axis] * value.irradiance[0][3];
          statistics.hitBounds[face].include(hit);
        }
        for (auto *part : {&statistics.total, &statistics.faces[face]}) {
          if (!missed && value.irradiance[0][3] <= exit * 1.05f)
            part->agreement += weight;
          part->total += weight;
          part->missed += weight * value.statistics[1];
          part->back += weight * value.statistics[0];
          if (!missed) {
            part->nearer += weight * (relative < -.05);
            part->farther += weight * (relative > .05);
            // Bound grazing-ray outliers; this diagnostic does not change selection.
            part->residual += weight * std::clamp(relative, -1., 4.);
          }
        }
      }
      m_seen[index] = 1;
      ++m_completed;
    }
    return true;
  }
  std::vector<FittedRegion> ProjectionAudit::finish() const {
    check(m_cancelled);
    if (m_completed != m_rays.size())
      throw std::logic_error("Reflection projection queries are incomplete");
    auto regions = m_regions;
    for (size_t i = 0; i < regions.size(); ++i) {
      check(m_cancelled);
      const auto &statistics = m_statistics[i].total;
      regions[i].quality = float(std::clamp(statistics.agreement / statistics.total, .05, 1.));
      regions[i].missFraction = float(statistics.missed / statistics.total);
      regions[i].backfaceFraction = float(statistics.back / statistics.total);
      regions[i].geometryEvaluated = true;
      for (uint32_t face = 0; face < 6; ++face) {
        const auto &part = m_statistics[i].faces[face];
        auto &output = regions[i].faces[face];
        output.solidAngleFraction = float(part.total / statistics.total);
        output.hitBounds = m_statistics[i].hitBounds[face];
        if (part.total <= 0)
          continue;
        output.supportFraction = float(part.agreement / part.total);
        output.nearFraction = float(part.nearer / part.total);
        output.farFraction = float(part.farther / part.total);
        output.missFraction = float(part.missed / part.total);
        output.backfaceFraction = float(part.back / part.total);
        const double hitWeight = part.total - part.missed;
        output.meanRelativeResidual = hitWeight > 0 ? float(part.residual / hitWeight) : 0;
      }
    }
    return regions;
  }
} // namespace remix::forward::probes
