#include "forward/material.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
  using remix::forward::Matrix;
  Matrix reference(const Matrix &model) {
    const auto inverse = remix::scene::inverse(model);
    Matrix result;
    for (uint32_t row = 0; row < 4; ++row)
      for (uint32_t column = 0; column < 4; ++column)
        result[column * 4 + row] = inverse[row * 4 + column];
    return result;
  }
  void require(bool valid, const char *message) {
    if (!valid)
      throw std::runtime_error(message);
  }
  void check(const Matrix &model) {
    require(remix::forward::normalMatrix(model) == reference(model),
            "Normal matrix preserves the complete inverse transpose");
  }
  void colorFactors() {
    remix::scene::Draw draw;
    draw.model = remix::scene::identity();
    draw.material = std::make_shared<remix::scene::Material>();
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t channels[] = {i, 255 - i, (i * 37) & 255, (i * 13) & 255};
      draw.blend.factor = channels[3] << 24 | channels[0] << 16 | channels[1] << 8 | channels[2];
      const auto actual = remix::forward::materialData(draw);
      for (uint32_t c = 0; c < 4; ++c) {
        const float encoded = float(channels[c]) / 255;
        const float expected = c == 3               ? encoded
                               : encoded <= .04045f ? encoded / 12.92f
                                                    : std::pow((encoded + .055f) / 1.055f, 2.4f);
        require(actual.factor[c] == expected, "All byte colours keep sRGB RGB and linear alpha semantics");
      }
    }
  }
  void zeroDiffuseEligibility() {
    using remix::forward::zeroDiffuseMaterial;
    remix::scene::Material m;
    require(!zeroDiffuseMaterial(m), "Ordinary diffuse materials keep the generic shader");
    m.metallic = 1;
    require(zeroDiffuseMaterial(m), "Constant full metal has no diffuse lobe");
    m.metallicTexture = std::make_shared<remix::scene::Texture>();
    require(!zeroDiffuseMaterial(m), "A metallic texture overrides the constant proof");
    m = {};
    m.selfIllumination = 1;
    require(zeroDiffuseMaterial(m), "Constant unmasked self illumination removes diffuse reflectance");
    m.selfIllumination = 3;
    require(zeroDiffuseMaterial(m), "Missing emission map supplies white emissive alpha");
    m.emissionTexture = std::make_shared<remix::scene::Texture>();
    require(!zeroDiffuseMaterial(m), "Animated or textured emission is not a draw-wide proof");
    m.emissionTexture.reset();
    m.selfIllumination = 2;
    require(!zeroDiffuseMaterial(m), "Albedo alpha may retain diffuse reflectance");
    m.selfIllumination = 1;
    m.selfIlluminationScale = .999f;
    require(!zeroDiffuseMaterial(m), "Partial self illumination stays diffuse");
    m.selfIlluminationScale = std::numeric_limits<float>::infinity();
    require(!zeroDiffuseMaterial(m), "Nonfinite self illumination cannot establish eligibility");
    m = {};
    m.tint[0] = m.tint[1] = m.tint[2] = 0;
    require(zeroDiffuseMaterial(m), "Black final tint removes diffuse for every blend argument");
    m.tint[2] = .001f;
    require(!zeroDiffuseMaterial(m), "Even a small remaining colour keeps ordinary diffuse lighting");
  }
  void benchmark(bool translated) {
    std::vector<Matrix> models;
    for (uint32_t i = 0; i < 1024; ++i) {
      auto model = remix::scene::identity();
      model[12] = float(int32_t(i) - 512) * 31.25f;
      model[13] = float(i % 47) * -13.5f;
      model[14] = float(i % 19) * 8.125f;
      if (!translated) {
        model[0] = 1.25f + .25f * (i % 3);
        model[5] = .75f;
        model[4] = .125f;
      }
      models.push_back(model);
    }
    // Alternating controls use identical inputs and observable complete outputs.
    for (bool optimized : {false, true, true, false}) {
      volatile double checksum = 0;
      const auto began = std::chrono::steady_clock::now();
      for (uint32_t pass = 0; pass < 1024; ++pass)
        for (const auto &model : models) {
          const auto normal = optimized ? remix::forward::normalMatrix(model) : reference(model);
          double value = 0;
          for (float component : normal)
            value += component;
          checksum = checksum + value;
        }
      std::printf("Normal matrix input %s, path %s: %.3f ms, checksum %.9g\n",
                  translated ? "translation" : "scale/shear", optimized ? "optimized" : "reference",
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count(),
                  double(checksum));
    }
  }
} // namespace
int main(int argc, char **argv) try {
  const float translations[] = {
      0, -0.f, .03125f, -127.125f, 16384, -16384, std::numeric_limits<float>::max()};
  for (float x : translations)
    for (float y : translations)
      for (float z : translations) {
        auto model = remix::scene::identity();
        model[12] = x;
        model[13] = y;
        model[14] = z;
        check(model);
      }
  for (uint32_t component = 0; component < 16; ++component) {
    auto model = remix::scene::identity();
    model[component] += .25f;
    check(model); // Scale, shear, translation and non-affine fallback.
  }
  colorFactors();
  zeroDiffuseEligibility();
  auto rotation = remix::scene::identity();
  rotation[0] = rotation[5] = 0;
  rotation[1] = 1;
  rotation[4] = -1;
  check(rotation);
  auto singular = remix::scene::identity();
  singular[0] = 0;
  bool refused = false;
  try {
    remix::forward::normalMatrix(singular);
  } catch (remixapi_ErrorCode error) {
    refused = error == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  require(refused, "Singular transforms retain their existing rejection");
  std::puts(
      "Normal matrices: translations, large coordinates, rotations, shear, scale and singular fallback pass");
  if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) {
    benchmark(true);
    benchmark(false);
  }
} catch (const std::exception &error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
