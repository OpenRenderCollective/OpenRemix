#include "forward/lightmap/seams.h"
#include "forward/lighting.h"
#include "forward/lightmap/coverage.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace remix::forward::lightmap {
  namespace {
    using D3 = std::array<double, 3>;
    using D2 = std::array<double, 2>;
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Surface seam planning superseded");
    }
    double dot3(D3 a, D3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    struct Segment {
      uint32_t chart;
      double first, last;
      D3 start, end;
    };
    bool collinear(const Segment &a, const Segment &b) {
      D3 direction;
      for (uint32_t c = 0; c < 3; ++c)
        direction[c] = a.end[c] - a.start[c];
      const double length2 = dot3(direction, direction);
      for (const auto &point : {b.start, b.end}) {
        D3 difference;
        for (uint32_t c = 0; c < 3; ++c)
          difference[c] = point[c] - a.start[c];
        const double along = dot3(difference, direction) / length2;
        double distance2 = 0;
        for (uint32_t c = 0; c < 3; ++c) {
          const double delta = difference[c] - along * direction[c];
          distance2 += delta * delta;
        }
        if (distance2 > 1e-8)
          return false; // 0.1 mm, independent of world units and luxel size
      }
      return true;
    }
    std::vector<uint32_t> patches(const scene::BakeScene &scene, const Layout &layout,
                                  std::stop_token cancelled) {
      std::vector<uint32_t> parent(layout.mappings.size());
      std::iota(parent.begin(), parent.end(), 0u);
      auto root = [&](uint32_t i) {
        while (parent[i] != i) {
          parent[i] = parent[parent[i]];
          i = parent[i];
        }
        return i;
      };
      // A line's orientation and perpendicular distance group collinear edges.
      // Shared subsegments count too, so T-junctions do not require equal endpoints.
      using Key = std::array<int64_t, 6>;
      std::map<Key, std::vector<Segment>> lines;
      for (uint32_t index = 0; index < layout.mappings.size(); ++index) {
        check(cancelled);
        const auto &mapping = layout.mappings[index];
        if (!mapping.worldGrid)
          continue;
        const auto &chart = scene.charts[index];
        const auto &draw = scene.draws[chart.draw];
        auto position = [&](size_t corner) {
          const auto vertex = int64_t(draw.surface->indices[chart.firstIndex + corner]) + draw.vertexOffset;
          const auto &p = draw.surface->vertices[size_t(vertex)].position;
          const auto world = transformPoint(draw.model, {p[0], p[1], p[2]});
          D3 result;
          for (uint32_t c = 0; c < 3; ++c)
            result[c] = double(world[c]) * scene.sceneScale;
          return result;
        };
        for (size_t triangle = 0; triangle < chart.texcoords.size(); triangle += 3)
          for (uint32_t edge = 0; edge < 3; ++edge) {
            const auto a = position(triangle + edge), b = position(triangle + (edge + 1) % 3);
            const D2 start{dot3(mapping.u, a), dot3(mapping.v, a)},
                end{dot3(mapping.u, b), dot3(mapping.v, b)};
            D2 direction{end[0] - start[0], end[1] - start[1]};
            const double length = std::hypot(direction[0], direction[1]);
            if (length <= .0001)
              continue;
            const uint32_t axis = std::abs(direction[0]) >= std::abs(direction[1]) ? 0 : 1;
            const double sign = direction[axis] < 0 ? -1 : 1;
            for (auto &c : direction)
              c *= sign / length;
            const double distance = direction[0] * start[1] - direction[1] * start[0];
            Key key{mapping.normalKey[0],
                    mapping.normalKey[1],
                    mapping.normalKey[2],
                    std::llround(direction[0] * 1000000),
                    std::llround(direction[1] * 1000000),
                    std::llround(distance * 10000)};
            const double first = direction[0] * start[0] + direction[1] * start[1],
                         last = direction[0] * end[0] + direction[1] * end[1];
            lines[key].push_back({index, std::min(first, last), std::max(first, last), a, b});
          }
      }
      for (auto &[key, segments] : lines)
        std::sort(segments.begin(), segments.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
      for (const auto &[key, segments] : lines) {
        check(cancelled);
        for (int offset : {-1, 0, 1}) {
          auto neighbour = key;
          neighbour[5] += offset;
          const auto found = lines.find(neighbour);
          if (found == lines.end())
            continue;
          for (const auto &a : segments) {
            check(cancelled);
            for (const auto &b : found->second) {
              if (b.first >= a.last - .0001)
                break;
              if (a.chart == b.chart || b.last <= a.first + .0001 || !collinear(a, b))
                continue;
              const auto first = root(a.chart), second = root(b.chart);
              parent[std::max(first, second)] = std::min(first, second);
            }
          }
        }
      }
      for (auto &p : parent)
        p = root(p);
      return parent;
    }
  } // namespace
  void extendSeams(const scene::BakeScene &scene, Layout &layout, uint32_t padding,
                   std::stop_token cancelled) {
    check(cancelled);
    if (layout.luxelMetres <= 0)
      return;
    const auto connected = patches(scene, layout, cancelled);
    Coverage coverage(scene, layout, connected, cancelled);
    using Key = std::tuple<uint32_t, int64_t, int64_t>;
    std::map<Key, bake::Point> centers;
    auto coordinate = [&](uint32_t chart, uint32_t atlas) {
      const auto &allocation = layout.charts[chart];
      const auto &mapping = layout.mappings[chart];
      const int64_t x = atlas % layout.pageSize - int64_t(allocation.x);
      const int64_t y = (atlas / layout.pageSize) % layout.pageSize - int64_t(allocation.y);
      return Key{connected[chart], mapping.origin[0] + x, mapping.origin[1] + y};
    };
    for (size_t i = 0; i < layout.points.size(); ++i) {
      if ((i & 1023u) == 0)
        check(cancelled);
      const auto chart = layout.pointCharts[i];
      if (layout.mappings[chart].worldGrid && layout.sampleCenters[i])
        centers.try_emplace(coordinate(chart, layout.addresses[i]), layout.points[i]);
    }
    // Snapshot values above, so extending one chart cannot move another donor.
    // Each added sample remains attributed to the receiving chart: mixed-light
    // ranking and visibility/direct coefficient channels retain their identities.
    for (uint32_t chart = 0; chart < layout.charts.size(); ++chart) {
      check(cancelled);
      if (!layout.mappings[chart].worldGrid)
        continue;
      const auto &a = layout.charts[chart];
      for (uint32_t y = a.y - padding; y < a.y + a.height + padding; ++y)
        for (uint32_t x = a.x - padding; x < a.x + a.width + padding; ++x) {
          const uint32_t atlas = (a.page * layout.pageSize + y) * layout.pageSize + x;
          const auto owner = layout.owners[atlas];
          if (!layout.footprints[atlas] || owner == unowned)
            continue;
          const auto found = centers.find(coordinate(chart, atlas));
          if (found == centers.end())
            continue;
          const auto &point = found->second;
          if (layout.points[owner].position == point.position && layout.points[owner].normal == point.normal)
            continue;
          auto projected = [&](const bake::Point &sample) {
            Coverage::Point uv{};
            const auto &mapping = layout.mappings[chart];
            for (uint32_t c = 0; c < 3; ++c) {
              const double metres = double(sample.position[c]) * scene.sceneScale;
              uv[0] += mapping.u[c] * metres / layout.luxelMetres;
              uv[1] += mapping.v[c] * metres / layout.luxelMetres;
            }
            return uv;
          };
          if (!coverage.covers(connected[chart], projected(layout.points[owner]), projected(point)))
            continue;
          if (layout.addresses[owner] == atlas && layout.pointCharts[owner] == chart &&
              layout.sampleCenters[owner]) {
            layout.points[owner] = point;
            layout.sampleCenters[owner] = 1;
          } else {
            layout.owners[atlas] = uint32_t(layout.points.size());
            layout.points.push_back(point);
            layout.pointCharts.push_back(chart);
            layout.addresses.push_back(atlas);
            layout.sampleCenters.push_back(1);
            layout.receiverSamples.push_back(0);
          }
          ++layout.seamSamples;
        }
    }
  }
} // namespace remix::forward::lightmap
