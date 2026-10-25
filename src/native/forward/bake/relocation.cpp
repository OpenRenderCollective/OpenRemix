#include "forward/bake/grid.h"
#include "forward/bake/relocation_cache.h"
#include "forward/bake/receiver_clearance.h"
#include "forward/lightmap/layout.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    std::optional<Vec4> receiverPosition(const scene::BakeScene &scene, const Point &receiver, float clearance) {
      const Vec3 normal = normalized({receiver.normal[0], receiver.normal[1], receiver.normal[2]});
      const auto contained = partitionContainedBias(scene,
          {receiver.position[0], receiver.position[1], receiver.position[2]}, normal, clearance);
      if (!contained) return {};
      Vec4 position = receiver.position;
      for (uint32_t c = 0; c < 3; ++c) position[c] += normal[c] * *contained;
      position[3] = 0;
      return position;
    }
  }
  RelocationCache::RelocationCache(const scene::BakeScene &scene, const lightmap::Layout &layout)
      : m_scene(scene), m_layout(layout), m_positions(layout.points.size()), m_states(layout.points.size()) {}

  void RelocationCache::clearance(float value) {
    if (!std::isfinite(value) || value < 0) throw std::invalid_argument("Invalid cached receiver clearance");
    if (value == m_clearance) return;
    m_clearance = value;
    std::fill(m_states.begin(), m_states.end(), 0);
  }
  std::optional<Vec4> RelocationCache::position(uint32_t receiver) {
    if (m_clearance < 0) throw std::logic_error("Receiver clearance was not configured");
    auto &state = m_states.at(receiver);
    if (state) ++m_hits;
    else {
      ++m_queries;
      const auto point = receiverPosition(m_scene, m_layout.points.at(receiver), m_clearance);
      state = point ? 1 : 2;
      if (point) m_positions[receiver] = *point;
    }
    return state == 1 ? std::optional<Vec4>(m_positions[receiver]) : std::nullopt;
  }
  uint32_t relocate(Grid &grid, const scene::BakeScene &scene, const lightmap::Layout &layout,
                     std::stop_token cancelled, RelocationCache *cache,
                     std::span<const uint32_t> sourceReceivers) {
    if (indexed(grid.header)) throw std::invalid_argument("Relocate probes before compacting their lattice");
    if (!scene.partition || !scene.partition->closed || layout.points.empty()) return 0;
    const auto &h = grid.header;
    if (grid.points.size() != h.dimensions[3] || grid.probes.size() != grid.points.size() ||
        (!layout.receiverSamples.empty() && layout.receiverSamples.size() != layout.points.size()) ||
        (cache && sourceReceivers.size() != layout.points.size()) ||
        (!cache && !sourceReceivers.empty()) ||
        !std::isfinite(scene.sceneScale) || scene.sceneScale <= 0)
      throw std::invalid_argument("Invalid probe relocation inputs");
    std::vector<uint8_t> occupied(grid.points.size());
    for (auto task : grid.tasks) occupied.at(task) = 1;
    std::vector<float> distance(grid.points.size(), std::numeric_limits<float>::infinity());
    std::vector<Vec4> candidates(grid.points.size());
    const float clearance = std::min(.25f / scene.sceneScale, h.originSpacing[3] * .1f);
    if (cache) cache->clearance(clearance);
    const float maximumOffset = h.originSpacing[3] * .45f;
    for (size_t point = 0; point < layout.points.size(); ++point) {
      if ((point & 1023u) == 0 && cancelled.stop_requested())
        throw std::runtime_error("Probe relocation superseded");
      if (!layout.receiverSamples.empty() && !layout.receiverSamples[point]) continue;
      const auto prepared = cache ? cache->position(sourceReceivers[point])
                                  : receiverPosition(scene, layout.points[point], clearance);
      if (!prepared) continue;
      Vec4 p = *prepared;
      std::array<uint32_t, 3> coordinate;
      bool inside = true;
      for (uint32_t c = 0; c < 3; ++c) {
        const float cell = std::round((p[c] - h.originSpacing[c]) / h.originSpacing[3]);
        if (!std::isfinite(cell) || cell < 0 || cell >= h.dimensions[c]) {
          inside = false;
          break;
        }
        coordinate[c] = uint32_t(cell);
      }
      if (!inside) continue;
      const uint32_t index = (coordinate[2] * h.dimensions[1] + coordinate[1]) * h.dimensions[0] + coordinate[0];
      if (occupied[index]) continue;
      float distance2 = 0;
      for (uint32_t c = 0; c < 3; ++c) {
        const float offset = p[c] - grid.points[index].position[c];
        inside &= std::abs(offset) <= maximumOffset;
        distance2 += offset * offset;
      }
      if (!inside || distance2 >= distance[index] || scene.partition->cluster(p.data()) < 0) continue;
      distance[index] = distance2;
      p[3] = 0;
      candidates[index] = p;
    }
    uint32_t relocated = 0;
    for (uint32_t index = 0; index < distance.size(); ++index) {
      if (!std::isfinite(distance[index])) continue;
      grid.points[index].position = candidates[index];
      grid.tasks.push_back(index);
      ++relocated;
    }
    if (relocated) {
      grid.header.settings[2] = 1;
      std::sort(grid.tasks.begin(), grid.tasks.end());
    }
    return relocated;
  }
  void storeProbe(Grid &grid, uint32_t index, const Result &value) {
    if (index >= grid.probes.size() || index >= grid.points.size())
      throw std::out_of_range("Invalid irradiance probe result");
    auto &probe = grid.probes[index];
    probe = value;
    if (grid.header.settings[2] > .5f) {
      uint32_t coordinate = index;
      for (uint32_t c = 0; c < 3; ++c) {
        const float original = grid.origins.empty()
            ? grid.header.originSpacing[c] + (coordinate % grid.header.dimensions[c]) * grid.header.originSpacing[3]
            : grid.origins.at(index).position[c];
        probe.irradiance[c][3] = grid.points[index].position[c] - original;
        coordinate /= grid.header.dimensions[c];
      }
    }
  }
} // namespace remix::forward::bake
