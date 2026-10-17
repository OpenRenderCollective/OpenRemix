#include "forward/probes/evaluation.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace remix::forward::probes {
  namespace {
    constexpr uint32_t raysPerRegion = 6 * ProjectionAudit::resolution * ProjectionAudit::resolution;
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
    m_rays.reserve(m_regions.size() * raysPerRegion);
    for (const auto &region : m_regions) {
      check(cancelled);
      const Capture capture{region.bounds, region.site.position, resolution, {}, {}};
      const auto points = probes::rays(capture);
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
      const size_t index = first + i, regionIndex = index / raysPerRegion;
      const auto &region = m_regions[regionIndex];
      const auto &direction = m_rays[index].normal;
      const double dominant =
          std::max({std::abs(direction[0]), std::abs(direction[1]), std::abs(direction[2])});
      const double weight = dominant * dominant * dominant; // cube-map solid-angle Jacobian
      float exit = std::numeric_limits<float>::max();
      for (uint32_t axis = 0; axis < 3; ++axis)
        if (std::abs(direction[axis]) > 1e-10f) {
          const float plane = direction[axis] > 0 ? region.bounds.maximum[axis] : region.bounds.minimum[axis];
          exit = std::min(exit, (plane - region.site.position[axis]) / direction[axis]);
        }
      const auto &value = results[i];
      auto &statistics = m_statistics[regionIndex];
      if (value.statistics[1] == 0 && value.irradiance[0][3] <= exit * 1.05f)
        statistics.agreement += weight;
      statistics.total += weight;
      statistics.missed += weight * value.statistics[1];
      statistics.back += weight * value.statistics[0];
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
      const auto &statistics = m_statistics[i];
      regions[i].quality = float(std::clamp(statistics.agreement / statistics.total, .05, 1.));
      regions[i].missFraction = float(statistics.missed / statistics.total);
      regions[i].backfaceFraction = float(statistics.back / statistics.total);
      regions[i].geometryEvaluated = true;
    }
    return regions;
  }
} // namespace remix::forward::probes
