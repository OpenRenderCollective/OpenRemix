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
  static float transformComponent(const Matrix &m, const Vec3 &v, uint32_t row) {
    return m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2];
  }
  Vec3 transformVector(const Matrix &m, const Vec3 &v) {
    return {transformComponent(m, v, 0), transformComponent(m, v, 1), transformComponent(m, v, 2)};
  }
  Vec3 transformPoint(const Matrix &m, const Vec3 &v) {
    auto p = transformVector(m, v);
    const float w = m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15];
    for (int i = 0; i < 3; ++i)
      p[i] = (p[i] + m[12 + i]) / w;
    return p;
  }
  DepthRange depthRangeFromInverseProjection(const Matrix &inverseProjection) {
    const float a = transformPoint(inverseProjection, {0, 0, 0})[2];
    const float b = transformPoint(inverseProjection, {0, 0, 1})[2];
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
    // World-space geometry already has finite, ordered bounds. Preserve them
    // directly when there is no model transform; no corner transforms are needed.
    static const Matrix identity = scene::identity();
    if (bounds.valid && model == identity) return bounds;
    if (bounds.valid && isAxisAlignedAffine(model)) {
      Bounds scaled;
      scaled.valid = true;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const float scale = model[axis * 5], offset = model[12 + axis];
        const float scaledMinimum = scale * bounds.minimum[axis];
        const float scaledMaximum = scale * bounds.maximum[axis];
        const float a = scaledMinimum + offset, b = scaledMaximum + offset;
        if (!std::isfinite(a) || !std::isfinite(b)) {
          scaled.valid = false;
          break;
        }
        scaled.minimum[axis] = std::min(a, b);
        scaled.maximum[axis] = std::max(a, b);
      }
      // Retain finite corners if an endpoint overflows.
      if (scaled.valid) return scaled;
    } else if (bounds.valid && model[3] == 0 && model[7] == 0 && model[11] == 0 && model[15] == 1) {
      Bounds affine;
      affine.valid = true;
      for (uint32_t row = 0; row < 3; ++row) {
        Vec3 low, high;
        // Each row is monotone along each input axis. Select the two
        // extremal corners and evaluate in the ordinary transform's order.
        for (uint32_t axis = 0; axis < 3; ++axis) {
          const bool reversed = model[axis * 4 + row] < 0;
          low[axis] = reversed ? bounds.maximum[axis] : bounds.minimum[axis];
          high[axis] = reversed ? bounds.minimum[axis] : bounds.maximum[axis];
        }
        affine.minimum[row] = transformComponent(model, low, row) + model[12 + row];
        affine.maximum[row] = transformComponent(model, high, row) + model[12 + row];
        if (!std::isfinite(affine.minimum[row]) || !std::isfinite(affine.maximum[row])) {
          affine.valid = false;
          break;
        }
      }
      if (affine.valid) return affine;
    }
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
  namespace {
    // Inward world-space planes: -w <= x,y <= w, 0 <= z <= w.
    Vec4 clipPlane(const Matrix &clip, uint32_t plane) {
      const uint32_t row = plane / 2;
      const float sign = plane % 2 ? -1.f : 1.f;
      Vec4 coefficients;
      for (uint32_t c = 0; c < 4; ++c)
        coefficients[c] = (plane == 4 ? 0.f : clip[c * 4 + 3]) + sign * clip[c * 4 + row];
      return coefficients;
    }
    // The positive AABB vertex maximizes signed distance. Touching and
    // crossing boxes survive, with room for floating-point error.
    bool outsideClipPlane(const Bounds &bounds, const Vec4 &coefficients) {
      double distance = coefficients[3], scale = std::abs(distance);
      for (uint32_t c = 0; c < 3; ++c) {
        const double term =
            double(coefficients[c]) * (coefficients[c] >= 0 ? bounds.maximum[c] : bounds.minimum[c]);
        distance += term;
        scale += std::abs(term);
      }
      return distance < -1e-5 * std::max(1., scale);
    }
  } // namespace
  ClipFrustum::ClipFrustum(const Matrix &clip) {
    for (uint32_t plane = 0; plane < planes.size(); ++plane)
      planes[plane] = clipPlane(clip, plane);
  }
  bool ClipFrustum::intersects(const Bounds &bounds) const {
    if (!bounds.valid) return true;
    for (const auto &plane : planes)
      if (outsideClipPlane(bounds, plane)) return false;
    return true;
  }
  bool intersectsClip(const Bounds &bounds, const Matrix &clip) {
    if (!bounds.valid) return true;
    for (uint32_t plane = 0; plane < 6; ++plane)
      if (outsideClipPlane(bounds, clipPlane(clip, plane))) return false;
    return true;
  }
  Cascades shadowCascades(const Matrix &inverseCamera, const Matrix &view, DepthRange range, Vec3 direction,
                          const Bounds &casters, float distance, uint32_t resolution) {
    Cascades result;
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
    struct ReceiverRay {
      Vec3 nearPoint, farPoint;
      float nearDepth, farDepth;
    };
    // Each split intersects the same four world-space camera rays.
    std::array<ReceiverRay, 4> rays;
    uint32_t rayIndex = 0;
    for (float sx : {-1.f, 1.f})
      for (float sy : {-1.f, 1.f}) {
        const auto a = transformPoint(inverseCamera, {sx, sy, 0});
        const auto b = transformPoint(inverseCamera, {sx, sy, .9999f});
        rays[rayIndex++] = {a, b, transformPoint(view, a)[2] * range.sign,
                                 transformPoint(view, b)[2] * range.sign};
      }
    struct ReceiverCorner {
      Vec3 world, light;
    };
    const auto receiverBoundary = [&](float depth) {
      std::array<ReceiverCorner, 4> boundary;
      for (uint32_t i = 0; i < rays.size(); ++i) {
        const auto &ray = rays[i];
        const float t = (depth - ray.nearDepth) / (ray.farDepth - ray.nearDepth);
        auto &point = boundary[i];
        for (uint32_t axis = 0; axis < 3; ++axis)
          point.world[axis] = ray.nearPoint[axis] + (ray.farPoint[axis] - ray.nearPoint[axis]) * t;
        point.light = transformPoint(basis, point.world);
      }
      return boundary;
    };
    auto previous = receiverBoundary(range.nearPlane);
    // Offscreen casters contribute the same light-space depth to every split.
    float casterNear = std::numeric_limits<float>::infinity();
    float casterFar = -casterNear;
    if (casters.valid)
      for (uint32_t corner = 0; corner < 8; ++corner) {
        Vec3 point;
        for (uint32_t axis = 0; axis < 3; ++axis)
          point[axis] = (corner & (1u << axis)) ? casters.maximum[axis] : casters.minimum[axis];
        const float depth = transformPoint(basis, point)[2];
        casterNear = std::min(casterNear, depth);
        casterFar = std::max(casterFar, depth);
      }
    for (uint32_t c = 0; c < cascadeCount; ++c) {
      const float fraction = float(c + 1) / cascadeCount;
      const float split = .65f * range.nearPlane * std::pow(cascadeFar / range.nearPlane, fraction) +
                          .35f * (range.nearPlane + (cascadeFar - range.nearPlane) * fraction);
      result.splits[c] = split;
      const auto next = receiverBoundary(split);
      Bounds box;
      std::array<Vec3, 8> receiverCorners;
      for (uint32_t i = 0; i < previous.size(); ++i) {
        receiverCorners[i * 2] = previous[i].world;
        receiverCorners[i * 2 + 1] = next[i].world;
        box.include(previous[i].light);
        box.include(next[i].light);
      }
      // A tight light-space AABB changes size as the camera turns. Fit a
      // rotation-invariant receiver sphere so snapping also stabilizes texel
      // size. Round outward to absorb small inverse-camera rounding changes.
      Vec3 receiverCentre{};
      for (const auto &p : receiverCorners)
        for (uint32_t axis = 0; axis < 3; ++axis) receiverCentre[axis] += p[axis] / 8;
      float radius = .005f;
      for (const auto &p : receiverCorners) {
        Vec3 delta;
        for (uint32_t axis = 0; axis < 3; ++axis) delta[axis] = p[axis] - receiverCentre[axis];
        radius = std::max(radius, std::sqrt(dot(delta, delta)));
      }
      const float quantum = std::ldexp(1.f, std::ilogb(radius) - 10);
      radius = std::ceil(radius / quantum) * quantum;
      const auto lightCentre = transformPoint(basis, receiverCentre);
      for (uint32_t axis = 0; axis < 2; ++axis) {
        box.minimum[axis] = lightCentre[axis] - radius;
        box.maximum[axis] = lightCentre[axis] + radius;
      }
      box.minimum[2] = std::min(box.minimum[2], casterNear);
      box.maximum[2] = std::max(box.maximum[2], casterFar);
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
      previous = next;
    }
    return result;
  }
  float coneInnerCosine(float outerCosine, float softness) {
    if (outerCosine <= -1.f) return outerCosine; // unrestricted point emitter
    return std::cos(std::acos(std::clamp(outerCosine, -1.f, 1.f)) *
                    (1.f - std::clamp(softness, 0.f, 1.f)));
  }
  LightRange localLightRange(const scene::SphereLight &light, float cutoff) {
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
    const float fade = std::sqrt(power * area / std::max(cutoff, 1e-8f));
    return {light.forwardCutoff > 0 ? std::min(fade, light.forwardCutoff) : fade, fade};
  }
} // namespace remix::forward
