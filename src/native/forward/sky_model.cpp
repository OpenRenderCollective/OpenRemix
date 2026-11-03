#include "forward/sky_model.h"
#include "forward/half.h"
#include "forward/options.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace remix::forward::sky_model {
  namespace {
    constexpr float pi = std::numbers::pi_v<float>;
    float smoothstep(float edge0, float edge1, float x) {
      const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.f, 1.f);
      return t * t * (3 - 2 * t);
    }
    Vec3 mix(const Vec3 &a, const Vec3 &b, float t) {
      return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
    }
    Vec3 scale(const Vec3 &a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
    Vec3 add(const Vec3 &a, const Vec3 &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
    Vec3 multiply(const Vec3 &a, const Vec3 &b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2]}; }
    // A clear-sky approximation rather than a scattering simulation: blue
    // zenith, paler horizon, warm forward glow, reddening as the sun lowers.
    Vec3 radiance(const Vec3 &d, const Vec3 &s) {
      const float day = smoothstep(-.12f, .2f, s[1]);
      const float low = 1 - smoothstep(0, .35f, s[1]);
      const float cosine = std::max(dot(d, s), 0.f);
      const Vec3 zenith{.16f, .36f, .85f}, horizon{.62f, .76f, .95f}, warm{1, .55f, .25f};
      auto above = [&](float up) {
        const float h = std::pow(1 - std::max(up, 0.f), 4.f);
        Vec3 base = mix(zenith, horizon, h);
        base = mix(base, scale(multiply(base, warm), 1.6f), low * h * (.4f + .6f * cosine));
        const float glow = .35f * std::pow(cosine, 8.f) + 1.2f * std::pow(cosine, 64.f) +
                           6 * std::pow(cosine, 512.f);
        return add(scale(base, .06f + .94f * day), scale(mix({1, .95f, .85f}, warm, low), glow * day));
      };
      if (d[1] >= 0)
        return above(d[1]);
      // Below the horizon: a dim, sky-lit ground that meets the horizon smoothly.
      const Vec3 ground = scale(multiply({.30f, .28f, .25f}, above(0)), .5f);
      return mix(above(0), ground, smoothstep(0, -.06f, d[1]));
    }
  } // namespace
  Matrix worldToDome(uint32_t upAxis) {
    // Rows map world axes to the dome frame; both rotations keep handedness.
    const float rows[3][3][3] = {{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
                                 {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
                                 {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}};
    const auto &r = rows[std::min(upAxis, 2u)];
    Matrix m = scene::identity();
    for (uint32_t row = 0; row < 3; ++row)
      for (uint32_t column = 0; column < 3; ++column)
        m[column * 4 + row] = r[row][column];
    return m;
  }
  scene::DistantLight effectiveSun(const scene::DistantLight &source, const Options &options, uint32_t upAxis) {
    if (!options.sunOverride)
      return source;
    scene::DistantLight sun = source;
    const float azimuth = options.sunAzimuth * pi / 180, elevation = options.sunElevation * pi / 180;
    // Toward the sun in the dome frame, then back to world space.
    const Vec3 local{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                     std::cos(elevation) * std::sin(azimuth)};
    const auto toWorld = scene::inverse(worldToDome(upAxis));
    const auto toward = normalized(transformVector(toWorld, local));
    for (uint32_t c = 0; c < 3; ++c) {
      sun.direction[c] = -toward[c];
      sun.radiance[c] = options.sunIntensity * options.sunColor[c];
    }
    sun.angularDiameterDegrees = options.sunAngularDiameter;
    sun.isDistant = true;
    return sun;
  }
  float radianceScale(const scene::DistantLight &sun, const Options &options) {
    if (!sun.isDistant)
      return .5f * options.skyIntensity;
    const float luminance = .2126f * sun.radiance[0] + .7152f * sun.radiance[1] + .0722f * sun.radiance[2];
    return .12f * std::max(luminance, 0.f) * options.skyIntensity;
  }
  std::vector<uint16_t> texels(const Vec3 &towardSun) {
    const auto s = normalized(towardSun);
    std::vector<uint16_t> result(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
      const float theta = (float(y) + .5f) / height * pi;
      for (uint32_t x = 0; x < width; ++x) {
        const float phi = ((float(x) + .5f) / width - .5f) * 2 * pi;
        const Vec3 d{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
        const auto value = radiance(d, s);
        auto *texel = &result[(size_t(y) * width + x) * 4];
        for (uint32_t c = 0; c < 3; ++c)
          texel[c] = half::encode(value[c]);
        texel[3] = half::encode(1);
      }
    }
    return result;
  }
} // namespace remix::forward::sky_model
