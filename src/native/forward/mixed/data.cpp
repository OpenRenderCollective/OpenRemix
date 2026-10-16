#include "forward/mixed/data.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>
namespace remix::forward::mixed {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Mixed light planning superseded");
    }
    bool sameShape(const scene::SphereLight &a, const scene::SphereLight &b) {
      for (uint32_t corner = 0; corner < 3; ++corner)
        if (!std::equal(std::begin(a.corners[corner]), std::end(a.corners[corner]), b.corners[corner]))
          return false;
      return std::equal(std::begin(a.position), std::end(a.position), b.position) && a.radius == b.radius &&
             a.triangle == b.triangle &&
             std::equal(std::begin(a.direction), std::end(a.direction), b.direction) &&
             a.coneCos == b.coneCos && a.softness == b.softness && a.focus == b.focus;
    }
  } // namespace
  uint32_t Bindings::match(const scene::SphereLight &light) const {
    if (!source || light.dynamic)
      return none;
    const auto [first, last] = identities.equal_range(light.id);
    for (auto entry = first; entry != last; ++entry)
      if (sameShape(source->lights[entry->second], light))
        return entry->second;
    return none;
  }
  Plan plan(std::shared_ptr<const scene::BakeScene> source, const lightmap::Layout &layout, float cutoff,
            std::stop_token cancelled) {
    check(cancelled);
    if (!source || source->charts.size() != layout.charts.size() ||
        layout.points.size() != layout.pointCharts.size() || !std::isfinite(cutoff) || cutoff <= 0 ||
        source->lights.size() >= (1u << 24))
      throw std::invalid_argument("Invalid mixed light inputs");
    Plan result;
    result.lights.source = std::move(source);
    result.charts.resize(result.lights.source->charts.size(), {none, none, none, none});
    result.directCharts = result.charts;
    result.occlusion =
        Occlusion(uint32_t(result.charts.size()), uint32_t(result.lights.source->lights.size()));
    result.remaining.resize(result.charts.size());
    result.next.resize(result.charts.size());
    std::vector<float> radii;
    radii.reserve(result.lights.source->lights.size());
    std::vector<bool> selected(result.lights.source->lights.size());
    for (uint32_t i = 0; i < result.lights.source->lights.size(); ++i) {
      const auto &light = result.lights.source->lights[i];
      radii.push_back(light.dynamic ? 0 : influenceRadius(light, cutoff));
    }
    for (uint32_t index = 0; index < result.charts.size(); ++index) {
      check(cancelled);
      const auto &chart = result.lights.source->charts[index];
      const auto &draw = result.lights.source->draws[chart.draw];
      Bounds bounds;
      for (size_t corner = 0; corner < chart.texcoords.size(); ++corner) {
        const auto vertex = int64_t(draw.surface->indices[chart.firstIndex + corner]) + draw.vertexOffset;
        const auto &p = draw.surface->vertices[size_t(vertex)].position;
        bounds.include(transformPoint(draw.model, {p[0], p[1], p[2]}));
      }
      std::vector<std::pair<float, uint32_t>> candidates;
      for (uint32_t light = 0; light < radii.size(); ++light) {
        if ((light & 1023u) == 0)
          check(cancelled);
        const auto &sourceLight = result.lights.source->lights[light];
        float distance2 = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          const float delta = sourceLight.position[c] -
                              std::clamp(sourceLight.position[c], bounds.minimum[c], bounds.maximum[c]);
          distance2 += delta * delta;
        }
        if (radii[light] > 0 && std::isfinite(radii[light]) && distance2 <= radii[light] * radii[light])
          candidates.emplace_back(radii[light] * radii[light] / std::max(distance2, 1e-8f), light);
      }
      std::sort(candidates.begin(), candidates.end(), [&](const auto &a, const auto &b) {
        if (a.first != b.first)
          return a.first > b.first;
        const auto aid = result.lights.source->lights[a.second].id,
                   bid = result.lights.source->lights[b.second].id;
        return std::tie(aid, a.second) < std::tie(bid, b.second);
      });
      for (uint32_t channel = 0; channel < std::min(size_t(channels), candidates.size()); ++channel) {
        const auto light = candidates[channel].second;
        result.charts[index][channel] = light;
        if (!selected[light]) {
          selected[light] = true;
          result.lights.identities.emplace(result.lights.source->lights[light].id, light);
        }
      }
      result.overflow += uint32_t(candidates.size() - std::min(size_t(channels), candidates.size()));
      if (result.overflow > 4u * 1024 * 1024)
        throw std::length_error("Mixed-light candidates exceed the planning budget");
      for (size_t candidate = channels; candidate < candidates.size(); ++candidate)
        result.remaining[index].push_back(candidates[candidate].second);
    }
    for (uint32_t point = 0; point < layout.points.size(); ++point) {
      if ((point & 1023u) == 0)
        check(cancelled);
      const auto chart = layout.pointCharts[point];
      if (chart >= result.charts.size())
        throw std::invalid_argument("Invalid surface point chart");
      for (uint32_t channel = 0; channel < channels; ++channel)
        if (const auto light = result.charts[chart][channel]; light != none)
          result.tasks.push_back({point, light, channel});
    }
    return result;
  }
} // namespace remix::forward::mixed
