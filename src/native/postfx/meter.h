#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace remix::postfx {
struct Size {
  uint32_t width, height;
  bool operator==(const Size &) const = default;
};
inline std::vector<Size> pyramid(Size size) {
  unsigned levels = 1;
  for (unsigned n = std::min(size.width, size.height) / 8; n > 1 && levels < 12;
       n /= 2)
    ++levels;
  std::vector<Size> result;
  for (unsigned i = 0; i < levels; ++i) {
    size = {std::max(size.width / 2, 1u), std::max(size.height / 2, 1u)};
    result.push_back(size);
  }
  return result;
}
inline float adaptation(float seconds, float rate) {
  return -std::expm1(-std::clamp(seconds, 0.f, .25f) * rate);
}
struct Meter {
  double logSum = 0, weightSum = 0;
  void sample(std::array<float, 3> radiance, std::array<float, 3> albedo,
              unsigned x, unsigned y, Size size, float centre) {
    double average = 0;
    for (unsigned i = 0; i < 3; ++i) {
      if (!std::isfinite(radiance[i]) || !std::isfinite(albedo[i]))
        return;
      average +=
          std::max(radiance[i], 0.f) * .18 / std::max(albedo[i], .05f) / 3;
    }
    const double dx = (2. * (x + .5) - size.width) / size.height;
    const double dy = (2. * (y + .5) - size.height) / size.height;
    const double r = std::clamp(std::hypot(dx, dy), 0., 1.);
    const double weight =
        1 - std::clamp(centre, 0.f, 1.f) * r * r * (3 - 2 * r);
    logSum += weight * std::log(std::max(average, 1e-4));
    weightSum += weight;
  }
};
struct Exposure {
  // GPU scene-radiance metering uses initialized=2 for retained empty frames;
  // history remains valid, but there is no current sample to adapt toward.
  float current = 1, target = 1, initialized = 0, limit = 0;
  void resolve(const Meter &meter, float middle, float minEv, float maxEv,
               float blend, bool reset = false) {
    if (!(meter.weightSum > 0) || !std::isfinite(meter.logSum)) {
      if (reset) initialized = 0;
      return;
    }
    const float wanted =
        middle /
        std::max(float(std::exp(meter.logSum / meter.weightSum)), 1e-4f);
    target = std::clamp(wanted, std::exp2(minEv), std::exp2(maxEv));
    limit = wanted < target ? -1 : wanted > target ? 1 : 0;
    current += (target - current) * (initialized && !reset ? blend : 1);
    initialized = 1;
  }
};
} // namespace remix::postfx
