#include "forward/bake/styled_direct.h"
#include "forward/mixed/directional_fields.h"
#include "forward/half.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    void require(bool valid) {
      if (!valid) throw std::invalid_argument("Invalid grouped direct lighting source");
    }
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Grouped direct lighting superseded");
    }
    void add(sparse::Coefficients &sum, const UnitDirect &unit, const scene::SphereLight &light) {
      for (uint32_t c = 0; c < 9; ++c)
        for (uint32_t rgb = 0; rgb < 3; ++rgb)
          sum[c][rgb] += unit[c] * light.radiance[rgb];
    }
  }
  StyledDirectGroups groupStyledDirect(const scene::BakeScene &source,
      const lightmap::Layout &layout, std::span<const mixed::DirectionalLink> owners,
      std::span<const uint32_t> directionals, const ProbeVisibility &probes,
      uint64_t maximumBytes, std::stop_token cancelled) {
    check(cancelled);
    require(source.lights.size() < (1u << 24) && layout.charts.size() < UINT32_MAX &&
        !probes.offsets().empty());
    mixed::DirectionalFields::validate(directionals, uint32_t(layout.charts.size()),
                                       uint32_t(source.lights.size()), directionals.size_bytes());
    const auto styles = styleIds(source);
    std::vector<std::vector<uint32_t>> chartLights(layout.charts.size());
    std::vector<uint8_t> probeLights(source.lights.size());
    const bool completeProbes = probes.direct().size() == probes.entries().size();
    for (const auto &entry : probes.entries()) {
      require(entry.light < source.lights.size());
      require(std::isfinite(entry.visibility) && (entry.visibility == -1 ||
              (entry.visibility >= 0 && entry.visibility <= 1)));
      // Unknown in one volume must not invalidate an unrelated style.
      if (entry.visibility < 0) probeLights[entry.light] = 2;
      else if (probeLights[entry.light] == 0) probeLights[entry.light] = 1;
    }
    std::map<uint32_t, uint32_t> missingSurfaceLight;
    for (size_t i = 0; i < owners.size(); ++i) {
      check(cancelled);
      const auto [chart, light] = owners[i];
      require(chart < layout.charts.size() && light < source.lights.size());
      if (i) require(owners[i - 1].chart < chart ||
                     (owners[i - 1].chart == chart && owners[i - 1].light < light));
      const auto &emitter = source.lights[light];
      if (!emitter.style) continue;
      chartLights[chart].push_back(light);
      if (!mixed::DirectionalFields::sample(directionals, chart, light, 0, 0))
        missingSurfaceLight.try_emplace(emitter.style, light);
    }
    // Cached layouts intentionally omit transient surface receivers. Captured
    // fields already include receiver coverage and seam donors; reconstruct
    // only their immutable atlas rectangles, not world samples or RGB arrays.
    std::vector<uint32_t> chartAtPixel(uint64_t(layout.pageSize) * layout.pageSize * layout.pages,
                                      lightmap::unowned);
    for (uint32_t chart = 0; chart < layout.charts.size(); ++chart) {
      check(cancelled);
      const auto &r = layout.charts[chart];
      require(r.page < layout.pages && r.width && r.height && r.width <= layout.pageSize &&
              r.height <= layout.pageSize && r.x <= layout.pageSize - r.width && r.y <= layout.pageSize - r.height);
      for (uint32_t y = r.y; y < r.y + r.height; ++y)
        for (uint32_t x = r.x; x < r.x + r.width; ++x) {
          auto &owner = chartAtPixel[(size_t(r.page) * layout.pageSize + y) * layout.pageSize + x];
          require(owner == lightmap::unowned);
          owner = chart;
        }
    }
    for (const auto &light : source.lights)
      for (float radiance : light.radiance) require(std::isfinite(radiance) && radiance >= 0);
    sparse::Builder builder({layout.pageSize, layout.pages, uint32_t(probes.offsets().size() - 1)}, maximumBytes);
    StyledDirectGroups result;
    for (uint32_t style : styles) {
      check(cancelled);
      if (const auto missing = missingSurfaceLight.find(style); missing != missingSurfaceLight.end()) {
        result.declined.push_back({style, StyledDirectDecline::SurfaceCoverage, missing->second});
        continue;
      }
      bool covered = completeProbes;
      uint32_t missingProbeLight = UINT32_MAX;
      std::vector<uint32_t> lights;
      for (uint32_t i = 0; i < source.lights.size(); ++i)
        if (source.lights[i].style == style) {
          lights.push_back(i);
          if (probeLights[i] != 1 && missingProbeLight == UINT32_MAX) missingProbeLight = i;
          covered &= probeLights[i] == 1;
        }
      if (!covered) {
        result.declined.push_back({style, StyledDirectDecline::ProbeCoverage, missingProbeLight});
        continue;
      }
      try {
        // Captured chart rectangles include their seam donors. Keep every tile
        // intersecting a chart owned by this style; only unrelated tiles are
        // proven black. Reuse no world-space or transient receiver assumptions.
        const uint32_t wide = (layout.pageSize + sparse::tileSide - 1) / sparse::tileSide;
        std::vector<uint8_t> occupied(size_t(wide) * wide * layout.pages);
        for (uint32_t chart = 0; chart < chartLights.size(); ++chart) {
          check(cancelled);
          const auto &lights = chartLights[chart];
          if (std::none_of(lights.begin(), lights.end(), [&](uint32_t light) {
                return source.lights[light].style == style;
              })) continue;
          const auto &r = layout.charts[chart];
          for (uint32_t y = r.y / sparse::tileSide; y <= (r.y + r.height - 1) / sparse::tileSide; ++y)
            for (uint32_t x = r.x / sparse::tileSide; x <= (r.x + r.width - 1) / sparse::tileSide; ++x)
              occupied[(size_t(r.page) * wide + y) * wide + x] = 1;
        }
        builder.addSampled(style, [&](uint32_t page, uint32_t x, uint32_t y) {
          sparse::Coefficients sum{};
          const uint32_t chart = chartAtPixel[(size_t(page) * layout.pageSize + y) * layout.pageSize + x];
          if (chart == lightmap::unowned) return sum;
          const auto &rectangle = layout.charts[chart];
          require(rectangle.page == page && rectangle.width && rectangle.height);
          const auto localX = uint32_t(std::clamp<int64_t>(int64_t(x) - rectangle.x, 0, rectangle.width - 1));
          const auto localY = uint32_t(std::clamp<int64_t>(int64_t(y) - rectangle.y, 0, rectangle.height - 1));
          for (uint32_t light : chartLights[chart]) {
            if (source.lights[light].style != style) continue;
            const auto field = mixed::DirectionalFields::sample(directionals, chart, light, localX, localY);
            require(field.has_value());
            UnitDirect unit;
            for (uint32_t c = 0; c < 9; ++c) unit[c] = half::decode((*field)[c]);
            add(sum, unit, source.lights[light]);
          }
          return sum;
        }, [&](uint32_t probe) {
          sparse::Coefficients sum{};
          for (uint32_t light : lights)
            if (const auto field = probes.sampleDirect(probe, light)) add(sum, *field, source.lights[light]);
          return sum;
        }, cancelled, [&](uint32_t tile) { return occupied[tile] != 0; });
        result.styles.push_back(style);
      } catch (const std::length_error &) {
        result.declined.push_back({style, StyledDirectDecline::Storage});
      }
    }
    result.words = builder.serialize(cancelled);
    return result;
  }
}
