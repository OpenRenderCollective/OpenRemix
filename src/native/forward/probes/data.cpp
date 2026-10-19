#include "forward/probes/data.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace remix::forward::probes {
  namespace {
    constexpr float pi = std::numbers::pi_v<float>;
    bool finite(Vec3 v) {
      return std::all_of(v.begin(), v.end(), [](float x) { return std::isfinite(x); });
    }
    void validate(const Capture &capture) {
      if (!validProjectionSpace(capture.worldToLocal) || capture.priority < -32768 || capture.priority > 32767)
        throw std::invalid_argument("Invalid reflection projection frame or priority");
      const auto origin = projectionPoint(capture.worldToLocal, capture.position);
      if (!finite(origin) || !capture.bounds.valid || !finite(capture.position) || !finite(capture.bounds.minimum) ||
          !finite(capture.bounds.maximum) || capture.resolution < 8 || capture.resolution > 128 ||
          !std::has_single_bit(capture.resolution))
        throw std::invalid_argument("Invalid reflection capture bounds or resolution");
      if (capture.influence.valid) {
        if (!finite(capture.influence.minimum) || !finite(capture.influence.maximum))
          throw std::invalid_argument("Invalid reflection influence bounds");
        for (uint32_t c = 0; c < 3; ++c)
          if (capture.influence.minimum[c] >= capture.influence.maximum[c] ||
              !std::isfinite(capture.influence.maximum[c] - capture.influence.minimum[c]) ||
              !std::isfinite(capture.influence.maximum[c] + capture.influence.minimum[c]))
            throw std::invalid_argument("Invalid reflection influence extent");
      }
      for (uint32_t c = 0; c < 3; ++c)
        if (capture.bounds.minimum[c] >= capture.bounds.maximum[c] ||
            !std::isfinite(capture.bounds.maximum[c] - capture.bounds.minimum[c]) ||
            !std::isfinite(capture.bounds.maximum[c] + capture.bounds.minimum[c]) ||
            origin[c] < capture.bounds.minimum[c] ||
            origin[c] > capture.bounds.maximum[c])
          throw std::invalid_argument("Reflection capture origin must lie inside its volume");
    }
    const Bounds &influence(const Capture &capture) {
      return capture.influence.valid ? capture.influence : capture.bounds;
    }
    Vec3 add(Vec3 a, Vec3 b, float weight) {
      for (uint32_t c = 0; c < 3; ++c)
        a[c] += b[c] * weight;
      return a;
    }
    Vec3 texel(std::span<const Vec4> pixels, uint32_t size, uint32_t face, int x, int y) {
      // Reproject taps that cross an edge instead of clamping them to the
      // original face. All mips use the same seam-aware lookup.
      if (x < 0 || y < 0 || x >= int(size) || y >= int(size)) {
        const auto adjacent =
            project(direction(face, 2 * (float(x) + .5f) / size - 1, 2 * (float(y) + .5f) / size - 1));
        face = adjacent.face;
        x = std::clamp(int((adjacent.uv[0] * .5f + .5f) * size), 0, int(size) - 1);
        y = std::clamp(int((adjacent.uv[1] * .5f + .5f) * size), 0, int(size) - 1);
      }
      const auto &p = pixels[(face * size + uint32_t(y)) * size + uint32_t(x)];
      return {p[0], p[1], p[2]};
    }
    Vec3 lookup(std::span<const Vec4> pixels, uint32_t size, Vec3 ray) {
      const auto face = project(ray);
      const float x = (face.uv[0] * .5f + .5f) * size - .5f;
      const float y = (face.uv[1] * .5f + .5f) * size - .5f;
      const int ix = int(std::floor(x)), iy = int(std::floor(y));
      const float fx = x - ix, fy = y - iy;
      Vec3 value{};
      for (uint32_t corner = 0; corner < 4; ++corner) {
        const bool right = corner & 1u, bottom = corner & 2u;
        value = add(value, texel(pixels, size, face.face, ix + right, iy + bottom),
                    (right ? fx : 1 - fx) * (bottom ? fy : 1 - fy));
      }
      return value;
    }
    float radicalInverse(uint32_t bits) {
      bits = (bits << 16) | (bits >> 16);
      bits = ((bits & 0x55555555u) << 1) | ((bits & 0xaaaaaaaau) >> 1);
      bits = ((bits & 0x33333333u) << 2) | ((bits & 0xccccccccu) >> 2);
      bits = ((bits & 0x0f0f0f0fu) << 4) | ((bits & 0xf0f0f0f0u) >> 4);
      bits = ((bits & 0x00ff00ffu) << 8) | ((bits & 0xff00ff00u) >> 8);
      return float(bits) * (1.f / 4294967296.f);
    }
    Vec3 prefilter(const Capture &capture, Vec3 n, float roughness, uint32_t samples) {
      const Vec3 up = std::abs(n[2]) < .99f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
      const Vec3 tangent = normalized(cross(up, n)), bitangent = cross(n, tangent);
      const float alpha = roughness * roughness, alphaSquared = alpha * alpha;
      Vec3 result{};
      float total = 0;
      for (uint32_t i = 0; i < samples; ++i) {
        const float phi = 2 * pi * float(i) / samples, xi = radicalInverse(i);
        const float cosine = std::sqrt((1 - xi) / (1 + (alphaSquared - 1) * xi));
        const float sine = std::sqrt(std::max(0.f, 1 - cosine * cosine));
        Vec3 h{};
        for (uint32_t c = 0; c < 3; ++c)
          h[c] = tangent[c] * sine * std::cos(phi) + bitangent[c] * sine * std::sin(phi) + n[c] * cosine;
        Vec3 light{};
        for (uint32_t c = 0; c < 3; ++c)
          light[c] = 2 * dot(n, h) * h[c] - n[c];
        const float weight = std::max(0.f, dot(n, light));
        if (weight <= 0)
          continue;
        result = add(result, lookup(capture.radiance, capture.resolution, light), weight);
        total += weight;
      }
      for (auto &channel : result)
        channel /= std::max(total, 1e-8f);
      return result;
    }
  } // namespace
  Vec3 direction(uint32_t face, float u, float v) {
    // Vulkan specification: Cube Map Face Selection and Transformations.
    switch (face) {
    case 0:
      return normalized({1, -v, -u});
    case 1:
      return normalized({-1, -v, u});
    case 2:
      return normalized({u, 1, v});
    case 3:
      return normalized({u, -1, -v});
    case 4:
      return normalized({u, -v, 1});
    case 5:
      return normalized({-u, -v, -1});
    default:
      throw std::invalid_argument("Invalid cube face");
    }
  }
  FaceUv project(Vec3 d) {
    if (!finite(d))
      throw std::invalid_argument("Non-finite cube direction");
    const float x = std::abs(d[0]), y = std::abs(d[1]), z = std::abs(d[2]);
    if (std::max({x, y, z}) < 1e-20f)
      throw std::invalid_argument("Zero cube direction");
    if (x >= y && x >= z)
      return d[0] >= 0 ? FaceUv{0, {-d[2] / x, -d[1] / x}} : FaceUv{1, {d[2] / x, -d[1] / x}};
    if (y >= z)
      return d[1] >= 0 ? FaceUv{2, {d[0] / y, d[2] / y}} : FaceUv{3, {d[0] / y, -d[2] / y}};
    return d[2] >= 0 ? FaceUv{4, {d[0] / z, -d[1] / z}} : FaceUv{5, {-d[0] / z, -d[1] / z}};
  }
  std::vector<bake::Point> rays(const Capture &capture) {
    validate(capture);
    const uint32_t size = capture.resolution;
    std::vector<bake::Point> result;
    result.reserve(6 * size * size);
    for (uint32_t face = 0; face < 6; ++face)
      for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
          const auto ray = direction(face, 2 * (float(x) + .5f) / size - 1, 2 * (float(y) + .5f) / size - 1);
          result.push_back({{capture.position[0], capture.position[1], capture.position[2], 0},
                            {ray[0], ray[1], ray[2], 0}});
        }
    return result;
  }
  uint32_t levelOffset(uint32_t resolution, uint32_t level) {
    uint32_t result = 0;
    for (uint32_t i = 0; i < level; ++i)
      result += 6 * (resolution >> i) * (resolution >> i);
    return result;
  }
  float projectionConfidence(const Capture &capture) {
    const uint32_t size = capture.resolution;
    if (!size || capture.radiance.size() != 6ull * size * size)
      return 1;
    double agreement = 0, total = 0;
    const auto origin = projectionPoint(capture.worldToLocal, capture.position);
    for (uint32_t face = 0; face < 6; ++face)
      for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) {
          const float u = 2 * (float(x) + .5f) / size - 1;
          const float v = 2 * (float(y) + .5f) / size - 1;
          const auto ray = projectionDirection(capture.worldToLocal, direction(face, u, v));
          const double weight = std::pow(1. + double(u) * u + double(v) * v, -1.5);
          float exit = std::numeric_limits<float>::max();
          for (uint32_t axis = 0; axis < 3; ++axis)
            if (std::abs(ray[axis]) > 1e-10f) {
              const float plane = ray[axis] > 0 ? capture.bounds.maximum[axis]
                                                : capture.bounds.minimum[axis];
              exit = std::min(exit, (plane - origin[axis]) / ray[axis]);
            }
          const float distance = capture.radiance[(face * size + y) * size + x][3];
          // The cube-map Jacobian avoids giving face corners disproportionate
          // influence. A relative margin covers the inset proxy and sampling
          // error without tying the result to metres or Source world units.
          // Nearer props are expected inside a room; only beyond-box distances
          // disagree. Missing depth in older/manual assets remains neutral.
          const bool unknown = !std::isfinite(distance) || distance <= 0;
          if (unknown || distance <= exit * 1.05f)
            agreement += weight;
          total += weight;
        }
    // A poorly approximated capture is still useful where it is the only one.
    // Positive weights keep normalized blending and HDR energy well-defined.
    return float(std::clamp(agreement / total, .05, 1.));
  }
  Record projectionRecord(const Capture &capture) {
    validate(capture);
    Record record;
    record.position[3] = projectionConfidence(capture);
    record.minimum[3] = float(capture.priority);
    record.worldToLocal = capture.worldToLocal;
    for (uint32_t c = 0; c < 3; ++c) {
      record.minimum[c] = capture.bounds.minimum[c];
      record.maximum[c] = capture.bounds.maximum[c];
      record.position[c] = capture.position[c];
      record.influenceMinimum[c] = influence(capture).minimum[c];
      record.influenceMaximum[c] = influence(capture).maximum[c];
    }
    return record;
  }
  Data filter(std::span<const Capture> captures, uint32_t samples, std::stop_token cancelled) {
    if (cancelled.stop_requested())
      throw std::runtime_error("Reflection filtering superseded");
    if (captures.size() > maxProbes || !samples || samples > 4096)
      throw std::invalid_argument("Invalid reflection probe/filter budget");
    Data result;
    if (captures.empty())
      return result;
    const uint32_t size = captures.front().resolution;
    for (const auto &capture : captures) {
      validate(capture);
      if (capture.resolution != size || capture.radiance.size() != 6ull * size * size)
        throw std::invalid_argument("Incomplete reflection capture faces");
      for (const auto &pixel : capture.radiance)
        for (uint32_t c = 0; c < 3; ++c)
          if (!std::isfinite(pixel[c]) || pixel[c] < 0 || pixel[c] > 65000)
            throw std::invalid_argument("Reflection capture radiance is outside the HDR range");
    }
    const uint32_t levels = std::bit_width(size), stride = levelOffset(size, levels);
    result.header.parameters = {uint32_t(captures.size()), size, levels, stride};
    result.texels.resize(captures.size() * stride);
    for (uint32_t probe = 0; probe < captures.size(); ++probe) {
      const auto &capture = captures[probe];
      result.header.probes[probe] = projectionRecord(capture);
      std::copy(capture.radiance.begin(), capture.radiance.end(), result.texels.begin() + probe * stride);
      for (uint32_t level = 1; level < levels; ++level) {
        const uint32_t mipSize = size >> level, start = probe * stride + levelOffset(size, level);
        for (uint32_t face = 0; face < 6; ++face)
          for (uint32_t y = 0; y < mipSize; ++y)
            for (uint32_t x = 0; x < mipSize; ++x) {
              if (cancelled.stop_requested())
                throw std::runtime_error("Reflection filtering superseded");
              const auto n =
                  direction(face, 2 * (float(x) + .5f) / mipSize - 1, 2 * (float(y) + .5f) / mipSize - 1);
              const auto color = prefilter(capture, n, float(level) / (levels - 1), samples);
              result.texels[start + (face * mipSize + y) * mipSize + x] = {color[0], color[1], color[2], 0};
            }
      }
    }
    return result;
  }
  std::vector<std::byte> serialize(const Data &data) {
    std::vector<std::byte> result(sizeof(Header) + data.texels.size() * sizeof(Vec4));
    std::memcpy(result.data(), &data.header, sizeof(Header));
    if (!data.texels.empty())
      std::memcpy(result.data() + sizeof(Header), data.texels.data(), data.texels.size() * sizeof(Vec4));
    return result;
  }
  std::optional<Vec3> sample(const Data &data, Vec3 position, Vec3 reflected, float roughness) {
    if (!data.header.parameters[0])
      return {};
    if (!finite(position) || !finite(reflected) || !std::isfinite(roughness))
      return {};
    const float lengthSquared = dot(reflected, reflected);
    if (!std::isfinite(lengthSquared) || lengthSquared < 1e-10f)
      return {};
    reflected = normalized(reflected);
    const auto &parameters = data.header.parameters;
    const float lod = std::clamp(roughness, 0.f, 1.f) * (parameters[2] - 1);
    const uint32_t low = uint32_t(lod), high = std::min(low + 1, parameters[2] - 1);
    Vec3 result{};
    float total = 0;
    float priority = -std::numeric_limits<float>::infinity();
    for (uint32_t probe = 0; probe < parameters[0]; ++probe) {
      const auto &record = data.header.probes[probe];
      const auto localPosition = projectionPoint(record.worldToLocal, position);
      const auto localRay = projectionDirection(record.worldToLocal, reflected);
      bool inside = true;
      float exitDistance = std::numeric_limits<float>::max(), edge = 1;
      float smallestHalf = std::numeric_limits<float>::max();
      for (uint32_t c = 0; c < 3; ++c) {
        inside &= localPosition[c] >= record.influenceMinimum[c] && localPosition[c] <= record.influenceMaximum[c];
        const float center = (record.influenceMinimum[c] + record.influenceMaximum[c]) * .5f;
        const float half = (record.influenceMaximum[c] - record.influenceMinimum[c]) * .5f;
        smallestHalf = std::min(smallestHalf, half);
        edge = std::min(edge, 1 - std::abs((localPosition[c] - center) / half));
        if (std::abs(localRay[c]) > 1e-10f) {
          const float plane = localRay[c] > 0 ? record.maximum[c] : record.minimum[c];
          exitDistance = std::min(exitDistance, (plane - localPosition[c]) / localRay[c]);
        }
      }
      if (!inside || record.minimum[3] < priority)
        continue;
      if (record.minimum[3] > priority) {
        priority = record.minimum[3];
        result = {};
        total = 0;
      }
      Vec3 corrected;
      for (uint32_t c = 0; c < 3; ++c)
        corrected[c] = position[c] + reflected[c] * exitDistance - record.position[c];
      if (exitDistance <= 0 || dot(corrected, corrected) < 1e-10f)
        corrected = reflected;
      auto mip = [&](uint32_t level) {
        const size_t start = probe * parameters[3] + levelOffset(parameters[1], level);
        const uint32_t size = parameters[1] >> level;
        return lookup(std::span(data.texels).subspan(start, 6 * size * size), size, corrected);
      };
      const auto a = mip(low), b = mip(high);
      // Several room captures can share a solid floor/wall boundary. Their
      // edge fades all reach zero there; capture distance preserves spatial
      // preference instead of averaging every touching capture equally. A
      // small room-relative core keeps the weight finite at capture origins.
      Vec3 offset;
      for (uint32_t c = 0; c < 3; ++c)
        offset[c] = position[c] - record.position[c];
      const float coreSquared = std::max(.01f * smallestHalf * smallestHalf, 1e-20f);
      const float confidence = record.position[3] > 0 ? record.position[3] : 1;
      const float weight = confidence * std::pow(std::max(.001f, edge), 2.f) /
                           std::max(dot(offset, offset), coreSquared);
      for (uint32_t c = 0; c < 3; ++c)
        result[c] += weight * std::lerp(a[c], b[c], lod - low);
      total += weight;
    }
    if (!total)
      return {};
    for (auto &channel : result)
      channel /= total;
    return result;
  }
} // namespace remix::forward::probes
