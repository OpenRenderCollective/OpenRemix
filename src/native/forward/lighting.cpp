#include "forward/lighting.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace remix::forward {
  uint32_t clusterListCapacity(uint32_t lights, uint32_t cells, uint64_t storageRange, uint32_t requested) {
    if (!cells) return clusterCapacity;
    const uint64_t budget = std::min(storageRange, uint64_t(32) << 20);
    const uint64_t stride = budget / (uint64_t(cells) * sizeof(uint32_t));
    if (stride <= clusterHeaderWords) return 1; // caller reports an unsupported grid
    const uint32_t target = requested ? std::clamp(requested, 1u, 1024u)
        : std::bit_ceil(std::clamp(lights, clusterCapacity, 512u));
    return uint32_t(std::min(uint64_t(target), stride - clusterHeaderWords));
  }
  float viewRadiusScale(const Matrix &view) {
    // The maximum absolute row sum of M^T M bounds its largest eigenvalue.
    // This also supports scaled/sheared hosts instead of assuming a rigid view.
    double maximum = 0;
    for (uint32_t row = 0; row < 3; ++row) {
      double sum = 0;
      for (uint32_t column = 0; column < 3; ++column) {
        double product = 0;
        for (uint32_t axis = 0; axis < 3; ++axis)
          product += double(view[row * 4 + axis]) * view[column * 4 + axis];
        sum += std::abs(product);
      }
      maximum = std::max(maximum, sum);
    }
    return std::nextafter(float(std::sqrt(maximum)), std::numeric_limits<float>::infinity());
  }
  Vec4 clusterSphereBounds(const Matrix &view, Vec3 position, float radius, float radiusScale) {
    const auto centre = transformVector(view, position);
    Vec4 result{centre[0] + view[12], centre[1] + view[13], centre[2] + view[14], radius * radiusScale};
    // Include the transform's absolute terms: camera-relative cancellation
    // can hide rounding error when a small light is far from the world origin.
    double magnitude = std::max(1.f, result[3]);
    for (uint32_t row = 0; row < 3; ++row) {
      double terms = std::abs(view[12 + row]);
      for (uint32_t column = 0; column < 3; ++column)
        terms += std::abs(double(view[column * 4 + row]) * position[column]);
      magnitude = std::max(magnitude, terms);
    }
    result[3] += float(1e-5 * magnitude);
    return result;
  }
  bool matchesSun(const scene::DistantLight &captured, const scene::DistantLight &live) {
    return captured.isDistant && live.isDistant &&
           captured.angularDiameterDegrees == live.angularDiameterDegrees &&
           std::equal(std::begin(captured.direction), std::end(captured.direction), live.direction);
  }
  float dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
  Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  }
  Vec3 normalized(Vec3 v) {
    const float length = std::sqrt(dot(v, v));
    if (!std::isfinite(length) || length < 1e-12f)
      return {0, 0, 1};
    for (float &c : v)
      c /= length;
    return v;
  }
  Vec3 transformVector(const Matrix &m, const Vec3 &v) {
    return {m[0] * v[0] + m[4] * v[1] + m[8] * v[2], m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
            m[2] * v[0] + m[6] * v[1] + m[10] * v[2]};
  }
  Vec3 transformPoint(const Matrix &m, const Vec3 &v) {
    auto p = transformVector(m, v);
    const float w = m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15];
    for (int i = 0; i < 3; ++i)
      p[i] = (p[i] + m[12 + i]) / w;
    return p;
  }
  DepthRange depthRange(const Matrix &projection) {
    const auto inverse = scene::inverse(projection);
    const float a = transformPoint(inverse, {0, 0, 0})[2];
    const float b = transformPoint(inverse, {0, 0, 1})[2];
    DepthRange range;
    range.sign = std::isfinite(a) && a < 0 ? -1.f : 1.f;
    range.nearPlane = std::isfinite(a) ? std::max(std::abs(a), .001f) : .1f;
    range.farPlane = std::isfinite(b) ? std::abs(b) : 1e6f;
    // Reverse-Z source cameras and orthographic cameras are valid inputs too.
    if (range.farPlane < range.nearPlane)
      std::swap(range.nearPlane, range.farPlane);
    range.nearPlane = std::max(range.nearPlane, .001f);
    range.farPlane = std::clamp(range.farPlane, range.nearPlane * 1.01f, 1e7f);
    return range;
  }
  uint32_t depthSlice(float depth, const DepthRange &range) {
    const float z = std::log(std::max(depth, range.nearPlane) / range.nearPlane) /
                    std::log(range.farPlane / range.nearPlane);
    return uint32_t(std::clamp(z * clusterSlices, 0.f, float(clusterSlices - 1)));
  }
  float sliceDepth(uint32_t slice, const DepthRange &range) {
    return range.nearPlane *
           std::pow(range.farPlane / range.nearPlane, float(std::min(slice, clusterSlices)) / clusterSlices);
  }
  void Bounds::include(const Vec3 &point) {
    for (float c : point)
      if (!std::isfinite(c))
        return;
    for (int i = 0; i < 3; ++i) {
      minimum[i] = valid ? std::min(minimum[i], point[i]) : point[i];
      maximum[i] = valid ? std::max(maximum[i], point[i]) : point[i];
    }
    valid = true;
  }
  void Bounds::merge(const Bounds &bounds) {
    if (bounds.valid) {
      include(bounds.minimum);
      include(bounds.maximum);
    }
  }
  Bounds transformBounds(const Bounds &bounds, const Matrix &model) {
    Bounds result;
    if (bounds.valid)
      for (uint32_t corner = 0; corner < 8; ++corner) {
        Vec3 point;
        for (uint32_t c = 0; c < 3; ++c)
          point[c] = (corner & (1u << c)) ? bounds.maximum[c] : bounds.minimum[c];
        result.include(transformPoint(model, point));
      }
    return result;
  }
  bool intersectsClip(const Bounds &bounds, const Matrix &clip) {
    if (!bounds.valid)
      return true;
    // Extract inward planes in world space: -w <= x,y <= w, 0 <= z <= w.
    // The positive AABB vertex maximizes each signed distance. Reject only
    // when even that vertex lies outside; touching and crossing boxes survive.
    for (uint32_t plane = 0; plane < 6; ++plane) {
      const uint32_t row = plane / 2;
      const float sign = plane % 2 ? -1.f : 1.f;
      Vec4 coefficients;
      for (uint32_t c = 0; c < 4; ++c)
        coefficients[c] = (plane == 4 ? 0.f : clip[c * 4 + 3]) + sign * clip[c * 4 + row];
      double distance = coefficients[3], scale = std::abs(distance);
      for (uint32_t c = 0; c < 3; ++c) {
        const double term =
            double(coefficients[c]) * (coefficients[c] >= 0 ? bounds.maximum[c] : bounds.minimum[c]);
        distance += term;
        scale += std::abs(term);
      }
      if (distance < -1e-5 * std::max(1., scale))
        return false;
    }
    return true;
  }
  Cascades shadowCascades(const Matrix &camera, const Matrix &view, DepthRange range, Vec3 direction,
                          const Bounds &casters, float distance, uint32_t resolution) {
    Cascades result;
    const auto inverse = scene::inverse(camera);
    const auto z = normalized(direction);
    const auto x = normalized(cross(std::abs(z[2]) < .99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0}, z));
    const auto y = cross(z, x);
    Matrix basis = scene::identity();
    for (int i = 0; i < 3; ++i) {
      basis[i * 4] = x[i];
      basis[i * 4 + 1] = y[i];
      basis[i * 4 + 2] = z[i];
    }
    const float cascadeFar = std::clamp(distance, range.nearPlane * 1.01f, range.farPlane);
    float previous = range.nearPlane;
    for (uint32_t c = 0; c < cascadeCount; ++c) {
      const float fraction = float(c + 1) / cascadeCount;
      const float split = .65f * range.nearPlane * std::pow(cascadeFar / range.nearPlane, fraction) +
                          .35f * (range.nearPlane + (cascadeFar - range.nearPlane) * fraction);
      result.splits[c] = split;
      Bounds box;
      for (float sx : {-1.f, 1.f})
        for (float sy : {-1.f, 1.f}) {
          const auto a = transformPoint(inverse, {sx, sy, 0});
          const auto b = transformPoint(inverse, {sx, sy, .9999f});
          const float az = transformPoint(view, a)[2] * range.sign;
          const float bz = transformPoint(view, b)[2] * range.sign;
          for (float depth : {previous, split}) {
            Vec3 p;
            const float t = (depth - az) / (bz - az);
            for (int i = 0; i < 3; ++i)
              p[i] = a[i] + (b[i] - a[i]) * t;
            box.include(transformPoint(basis, p));
          }
        }
      // All submitted casters contribute Z bounds, including off-screen ones.
      // XY remains fitted to this cascade to keep texel density useful.
      if (casters.valid)
        for (uint32_t corner = 0; corner < 8; ++corner) {
          Vec3 p;
          for (int i = 0; i < 3; ++i)
            p[i] = (corner & (1u << i)) ? casters.maximum[i] : casters.minimum[i];
          const float depth = transformPoint(basis, p)[2];
          box.minimum[2] = std::min(box.minimum[2], depth);
          box.maximum[2] = std::max(box.maximum[2], depth);
        }
      Matrix projection = scene::identity();
      for (int i = 0; i < 3; ++i) {
        float span = std::max(box.maximum[i] - box.minimum[i], .01f);
        float center = (box.minimum[i] + box.maximum[i]) * .5f;
        if (i < 2) {
          // One texel of padding keeps snapping from cropping the receiver.
          span *= float(resolution) / std::max(1.f, float(resolution) - 2);
          const float texel = span / std::max(1u, resolution);
          center = std::floor(center / texel) * texel;
        } else
          span += std::max(.01f, span * .01f);
        projection[i * 4 + i] = (i == 2 ? 1.f : 2.f) / span;
        projection[12 + i] = -center * projection[i * 4 + i] + (i == 2 ? .5f : 0.f);
      }
      result.matrices[c] = scene::multiply(projection, basis);
      previous = split;
    }
    return result;
  }
  float influenceRadius(const scene::SphereLight &light, float cutoff) {
    const float power = std::max({light.radiance[0], light.radiance[1], light.radiance[2], 0.f});
    float area = light.radius * light.radius * 3.14159265359f;
    if (light.triangle) {
      Vec3 a, b;
      for (int i = 0; i < 3; ++i) {
        a[i] = light.corners[1][i] - light.corners[0][i];
        b[i] = light.corners[2][i] - light.corners[0][i];
      }
      const auto n = cross(a, b);
      area = .5f * std::sqrt(dot(n, n));
    }
    return std::sqrt(power * area / std::max(cutoff, 1e-8f));
  }
} // namespace remix::forward
