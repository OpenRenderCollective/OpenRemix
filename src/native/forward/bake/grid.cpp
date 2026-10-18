#include "forward/bake/grid.h"
#include "forward/bake/spherical_harmonics.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    std::optional<UInt4> dimensions(const Bounds &bounds, double spacing, uint32_t budget) {
      UInt4 result{};
      uint64_t count = 1;
      for (uint32_t c = 0; c < 3; ++c) {
        const double size = std::ceil((double(bounds.maximum[c]) - bounds.minimum[c]) / spacing) + 3;
        if (!std::isfinite(size) || size < 3 || size > budget / count)
          return {};
        result[c] = uint32_t(size);
        count *= result[c];
      }
      result[3] = uint32_t(count);
      return result;
    }
    float fitSpacing(const Bounds &bounds, double requested, uint32_t budget) {
      if (!std::isfinite(requested) || requested <= 0 || requested > std::numeric_limits<float>::max())
        throw std::invalid_argument("Invalid probe spacing in world units");
      double lower = requested, upper = requested;
      if (!dimensions(bounds, requested, budget)) {
        // Establish a fitting bound, then use the available budget between
        // coarse powers of two. The requested density is always the minimum.
        do {
          upper *= 2;
          if (!std::isfinite(upper) || upper > std::numeric_limits<float>::max())
            throw std::invalid_argument("Irradiance bounds exceed world-coordinate precision");
        } while (!dimensions(bounds, upper, budget));
        for (uint32_t iteration = 0; iteration < 40; ++iteration) {
          const double middle = lower + (upper - lower) * .5;
          if (dimensions(bounds, middle, budget))
            upper = middle;
          else
            lower = middle;
        }
      }
      // Count cells using the spacing actually uploaded to the shader. Rounding
      // downward at a ceil boundary could otherwise exceed the allocation cap.
      float spacing = float(upper);
      if (double(spacing) < upper)
        spacing = std::nextafter(spacing, std::numeric_limits<float>::infinity());
      if (!std::isfinite(spacing) || spacing <= 0 || !dimensions(bounds, spacing, budget))
        throw std::invalid_argument("Irradiance grid cannot fit the probe budget");
      return spacing;
    }
  } // namespace
  Grid place(const scene::BakeScene &scene, const Config &config) {
    if (!std::isfinite(scene.sceneScale) || scene.sceneScale <= 0 || !std::isfinite(config.spacing) ||
        config.spacing <= 0 || config.maxProbes < 64 || config.maxProbes > (1u << 20))
      throw std::invalid_argument("Invalid irradiance grid spacing or probe limit");
    Bounds bounds;
    if (scene.hasBounds) {
      bounds.include(scene.minimum);
      bounds.include(scene.maximum);
    } else
      for (const auto &draw : scene.draws)
        for (const auto &vertex : draw.surface->vertices)
          bounds.include(
              transformPoint(draw.model, {vertex.position[0], vertex.position[1], vertex.position[2]}));
    if (!bounds.valid)
      throw std::invalid_argument("A bake needs probe bounds or static geometry bounds");
    for (uint32_t c = 0; c < 3; ++c)
      if (!std::isfinite(bounds.minimum[c]) || !std::isfinite(bounds.maximum[c]) ||
          bounds.minimum[c] > bounds.maximum[c])
        throw std::invalid_argument("Invalid irradiance bounds");
    const double spacing = fitSpacing(bounds, double(config.spacing) / scene.sceneScale, config.maxProbes);
    Grid result;
    result.header.dimensions = *dimensions(bounds, spacing, config.maxProbes);
    const uint32_t count = result.header.dimensions[3];
    for (uint32_t c = 0; c < 3; ++c) {
      const double lower = double(bounds.minimum[c]) - spacing;
      if (!std::isfinite(float(lower)))
        throw std::invalid_argument("Irradiance bounds exceed world-coordinate precision");
      result.header.originSpacing[c] = float(lower);
    }
    result.header.originSpacing[3] = float(spacing);
    if (!std::isfinite(float(spacing * spacing)))
      throw std::invalid_argument("Probe distance moments exceed float precision");
    result.header.settings = {float(spacing * .02), float(spacing * spacing * .001), 0, 0};
    result.points.resize(count);
    result.probes.resize(count);
    const auto &dimensions = result.header.dimensions;
    for (uint32_t z = 0; z < dimensions[2]; ++z)
      for (uint32_t y = 0; y < dimensions[1]; ++y)
        for (uint32_t x = 0; x < dimensions[0]; ++x) {
          const uint32_t index = (z * dimensions[1] + y) * dimensions[0] + x;
          auto &p = result.points[index].position;
          p = {result.header.originSpacing[0] + x * float(spacing),
               result.header.originSpacing[1] + y * float(spacing),
               result.header.originSpacing[2] + z * float(spacing), 0};
          for (float component : p)
            if (!std::isfinite(component))
              throw std::invalid_argument("Irradiance bounds exceed world-coordinate precision");
          if (!scene.partition || !scene.partition->closed || scene.partition->cluster(p.data()) >= 0)
            result.tasks.push_back(index);
        }
    if (result.tasks.empty())
      throw std::invalid_argument("Probe bounds contain no usable BSP leaves");
    return result;
  }
  static std::optional<Vec3> sampleSignal(const Grid &grid, Vec3 position, Vec3 normal, bool sunlight) {
    const auto &h = grid.header;
    if (!h.dimensions[3])
      return {};
    normal = normalized(normal);
    std::array<uint32_t, 3> base;
    Vec3 fraction;
    for (uint32_t c = 0; c < 3; ++c) {
      position[c] += normal[c] * h.settings[0];
      const float q = (position[c] - h.originSpacing[c]) / h.originSpacing[3];
      if (q < 0 || q > float(h.dimensions[c] - 1))
        return {};
      base[c] = std::min(uint32_t(q), h.dimensions[c] - 2);
      fraction[c] = q - base[c];
    }
    const auto sh = irradianceBasis(normal);
    Vec3 value{};
    float total = 0;
    for (uint32_t corner = 0; corner < 8; ++corner) {
      std::array<uint32_t, 3> coordinate;
      Vec3 delta;
      float weight = 1;
      for (uint32_t c = 0; c < 3; ++c) {
        const bool upper = corner & (1u << c);
        coordinate[c] = base[c] + upper;
        weight *= upper ? fraction[c] : 1 - fraction[c];
        delta[c] = position[c] - (h.originSpacing[c] + coordinate[c] * h.originSpacing[3]);
      }
      const auto &probe =
          grid.probes[(coordinate[2] * h.dimensions[1] + coordinate[1]) * h.dimensions[0] + coordinate[0]];
      if (h.settings[2] > .5f)
        for (uint32_t c = 0; c < 3; ++c) delta[c] -= probe.irradiance[c][3];
      if (probe.statistics[3] < .5f || weight <= 0)
        continue;
      const float distance = std::sqrt(dot(delta, delta));
      uint32_t axis = 0;
      for (uint32_t c = 1; c < 3; ++c)
        if (std::abs(delta[c]) > std::abs(delta[axis]))
          axis = c;
      const auto &moments = probe.moments[axis * 2 + uint32_t(delta[axis] < 0)];
      if (moments[3] <= 0)
        continue;
      if (distance > moments[0]) {
        const float variance = std::max(h.settings[1], moments[1] - moments[0] * moments[0]);
        const float difference = distance - moments[0];
        weight *= std::pow(variance / (variance + difference * difference), 3.f);
      }
      const float facing = distance > 1e-6f ? dot(normal, delta) / distance : -1;
      weight *= std::pow(std::max(.05f, .5f - .5f * facing), 2.f);
      if (sunlight) {
        if (probe.irradiance[7][3] < .5f && weight > 1e-6f)
          return {};
        value[0] += weight * probe.irradiance[8][3];
      } else {
        for (uint32_t j = 0; j < 9; ++j)
          for (uint32_t c = 0; c < 3; ++c)
            value[c] += weight * probe.irradiance[j][c] * sh[j];
      }
      total += weight;
    }
    if (total < 1e-4f)
      return {};
    for (auto &channel : value)
      channel = std::max(0.f, channel / total);
    return value;
  }
  std::optional<Vec3> sample(const Grid &grid, Vec3 position, Vec3 normal) {
    return sampleSignal(grid, position, normal, false);
  }
  std::optional<float> sampleSunVisibility(const Grid &grid, Vec3 position, Vec3 normal) {
    const auto value = sampleSignal(grid, position, normal, true);
    return value ? std::optional(std::clamp((*value)[0], 0.f, 1.f)) : std::nullopt;
  }
} // namespace remix::forward::bake
