#include "upscale/camera.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
} // namespace
int main() {
  for (float handedness : {-1.f, 1.f})
    for (auto planes : {remix::upscale::DepthRange{.01f, 100000.f}, {.05f, 512.f}, {.25f, 400000.f}}) {
      remix::scene::Matrix projection{};
      projection[0] = 1;
      projection[5] = -1;
      projection[10] = handedness * planes.farPlane / (planes.farPlane - planes.nearPlane);
      projection[11] = handedness;
      projection[14] = -planes.nearPlane * std::abs(projection[10]);
      const auto range = remix::upscale::depthRange(projection);
      const auto reversed = remix::upscale::reversedDepth(projection);
      require(std::isfinite(range.farPlane) && range.farPlane > range.nearPlane, "Invalid capture depth range");
      // These are FSR's inverted-depth reconstruction coefficients. Compare
      // recovered physical distances, including the SDK's FP32 arithmetic.
      const float q = range.nearPlane / (range.farPlane - range.nearPlane);
      const float offset = q * range.farPlane;
      double maximum = 0;
      for (unsigned i = 0; i <= 1000; ++i) {
        const float distance = range.nearPlane * std::pow(.95f * range.farPlane / range.nearPlane, i / 1000.f);
        const float viewZ = distance * handedness;
        const float depth = (reversed[10] * viewZ + reversed[14]) / (reversed[11] * viewZ);
        const float recovered = offset / (depth + q);
        maximum = std::max(maximum, double(std::abs(recovered - distance) / distance));
        require(depth >= 0 && depth <= 1.000001f, "Inverted depth outside clip range");
      }
      std::printf("handedness=%g near=%g far=%g maximum relative depth error=%g\n", handedness, range.nearPlane,
                  range.farPlane, maximum);
      require(maximum < 1e-5, "FSR loses capture depth precision or uses "
                              "mismatched camera endpoints");
      // Plane extraction must represent the submitted projection itself,
      // whose original authored far plane may already have rounded in FP32.
      const double farClip = (double(projection[10]) * range.farPlane * handedness + projection[14]) /
                             (double(projection[11]) * range.farPlane * handedness);
      require(std::abs(farClip - 1) < 1e-10, "Extracted far plane differs from submitted projection");
    }
}
