#include "forward/material.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace remix::forward {
  static const std::array<float, 256> &linearBytes() {
    static const auto values = [] {
      std::array<float, 256> result;
      for (uint32_t i = 0; i < result.size(); ++i) {
        const float c = float(i) / 255;
        result[i] = c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
      }
      return result;
    }();
    return values;
  }
  Matrix normalMatrix(const Matrix &model) {
    // Static world draws commonly use identity linear transforms. Preserve the
    // complete inverse transpose, including its unused translation row, without
    // running the general pivoted inverse for every draw.
    if (isTranslation(model)) {
      auto result = scene::identity();
      result[3] = 0.f - model[12];
      result[7] = 0.f - model[13];
      result[11] = 0.f - model[14];
      return result;
    }
    const auto validScale = [](float scale) {
      return std::isfinite(scale) && std::abs(double(scale)) >= 1e-15;
    };
    if (isAxisAlignedAffine(model) && validScale(model[0]) && validScale(model[5]) && validScale(model[10])) {
      auto result = scene::identity();
      // Match the general inverse's double-precision division and elimination.
      for (size_t axis = 0; axis < 3; ++axis) {
        const size_t diagonal = axis * 5;
        const double reciprocal = 1.0 / double(model[diagonal]);
        result[diagonal] = float(reciprocal);
        result[axis * 4 + 3] = float(0.0 - double(model[12 + axis]) * reciprocal);
      }
      return result;
    }
    const auto inverse = scene::inverse(model);
    Matrix result;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        result[c * 4 + r] = inverse[r * 4 + c];
    return result;
  }
  DrawData materialData(const scene::Draw &draw, double seconds) {
    const auto &m = *draw.material;
    DrawData data;
    data.model = draw.model;
    data.normal = normalMatrix(draw.model);
    std::copy_n(m.tint, 4, data.tint.begin());
    uint32_t animationFrame = 0;
    data.uv = scene::surfaceTexcoord(draw, seconds, animationFrame);
    data.pbr = {m.roughness, m.metallic, m.normalTexture && m.normalTexture->pixels.twoChannel ? 1.f : 0.f,
                acceptsSurfaceLayer(draw) ? 1.f : 0.f};
    std::copy_n(m.emission, 3, data.emission.begin());
    if (m.selfIllumination)
      data.emission[3] = m.selfIlluminationScale;
    const auto &b = draw.blend;
    data.state[0] = (b.colorOp & 3u) | (b.colorOp & 4u) << 10 | (b.alphaOpTexture & 4u) << 11 |
                    b.color1 << 2 | b.color2 << 4 | (b.alphaOpTexture & 3u) << 6 | b.alpha1 << 8 |
                    b.alpha2 << 10;
    data.state[1] = alphaComparison(draw);
    data.state[2] = std::bit_cast<uint32_t>(draw.alphaOverride ? draw.reference : m.reference);
    data.state[3] = uint32_t(draw.ignoreAlpha) | uint32_t(draw.doubleSided) << 1 |
                    (m.selfIllumination ? uint32_t(m.selfIllumination - 1) << 2 : 0) |
                    uint32_t(draw.cameraCull & REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT) << 4;
    const auto &linear = linearBytes();
    data.factor = {linear[(b.factor >> 16) & 255], linear[(b.factor >> 8) & 255],
                   linear[b.factor & 255], float(b.factor >> 24) / 255};
    data.maps[0] = uint32_t(bool(m.roughnessTexture)) | uint32_t(bool(m.metallicTexture)) << 1 |
                   uint32_t(bool(m.normalTexture)) << 2 | uint32_t(bool(m.emissionTexture)) << 3;
    const auto encoded = [](const std::shared_ptr<scene::Texture> &texture) {
      return texture && texture->pixels.srgb && texture->pixels.hdrMips.empty();
    };
    data.maps[0] |= uint32_t(encoded(m.texture)) << 5 | uint32_t(encoded(m.emissionTexture)) << 6 |
                    uint32_t(!m.pbr || m.suppressDirectSpecular) << 7;
    data.maps[1] = m.normalEncoding;
    for (uint32_t i = 0; i < 2; ++i) {
      const auto &r = m.remap[i];
      data.remap[i] = {r.exponent, r.bias, r.scale, 0};
      data.maps[2 + i] = r.channel | uint32_t(r.invert) << 3 | uint32_t(r.curve) << 4;
    }
    return data;
  }
} // namespace remix::forward
