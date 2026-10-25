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
    if (model[0] == 1 && model[5] == 1 && model[10] == 1 && model[15] == 1 &&
        model[1] == 0 && model[2] == 0 && model[3] == 0 && model[4] == 0 &&
        model[6] == 0 && model[7] == 0 && model[8] == 0 && model[9] == 0 && model[11] == 0 &&
        std::isfinite(model[12]) && std::isfinite(model[13]) && std::isfinite(model[14])) {
      auto result = scene::identity();
      result[3] = 0.f - model[12];
      result[7] = 0.f - model[13];
      result[11] = 0.f - model[14];
      return result;
    }
    const auto inverse = scene::inverse(model);
    Matrix result;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        result[c * 4 + r] = inverse[r * 4 + c];
    return result;
  }
  bool zeroDiffuseMaterial(const scene::Material &m) {
    if (!m.metallicTexture && m.metallic == 1) return true;
    if (m.tint[0] == 0 && m.tint[1] == 0 && m.tint[2] == 0) return true;
    // No emission map supplies the constant white texel in emissionLobes.
    // Albedo-alpha self illumination remains spatially varying and is refused.
    return !m.emissionTexture && (m.selfIllumination == 1 || m.selfIllumination == 3) &&
           std::isfinite(m.selfIlluminationScale) && m.selfIlluminationScale >= 1;
  }
  DrawData materialData(const scene::Draw &draw, double seconds) {
    const auto &m = *draw.material;
    DrawData data;
    data.model = draw.model;
    data.normal = normalMatrix(draw.model);
    std::copy_n(m.tint, 4, data.tint.begin());
    const uint32_t frame = uint32_t(std::fmod(seconds * m.fps, uint32_t(m.rows) * m.columns));
    data.uv = {1.f / m.columns, 1.f / m.rows, float(frame % m.columns) / m.columns,
               float(frame / m.columns) / m.rows};
    data.pbr = {m.roughness, m.metallic, m.normalTexture && m.normalTexture->pixels.twoChannel ? 1.f : 0.f,
                acceptsSurfaceLayer(draw) ? 1.f : 0.f};
    std::copy_n(m.emission, 3, data.emission.begin());
    if (m.selfIllumination)
      data.emission[3] = m.selfIlluminationScale;
    const auto &b = draw.blend;
    data.state[0] = (b.colorOp & 3u) | (b.colorOp & 4u) << 10 | (b.alphaOpTexture & 4u) << 11 |
                    b.color1 << 2 | b.color2 << 4 | (b.alphaOpTexture & 3u) << 6 | b.alpha1 << 8 |
                    b.alpha2 << 10;
    data.state[1] = draw.ignoreAlpha ? 7 : draw.alphaOverride ? draw.compare : m.compare;
    data.state[2] = std::bit_cast<uint32_t>(draw.alphaOverride ? draw.reference : m.reference);
    data.state[3] = uint32_t(draw.ignoreAlpha) | uint32_t(draw.doubleSided) << 1 |
                    (m.selfIllumination ? uint32_t(m.selfIllumination - 1) << 2 : 0) |
                    uint32_t(draw.cameraCull & REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT) << 4;
    const auto &linear = linearBytes();
    data.factor = {linear[(b.factor >> 16) & 255], linear[(b.factor >> 8) & 255],
                   linear[b.factor & 255], float(b.factor >> 24) / 255};
    data.maps[0] = uint32_t(bool(m.roughnessTexture)) | uint32_t(bool(m.metallicTexture)) << 1 |
                   uint32_t(bool(m.normalTexture)) << 2 | uint32_t(bool(m.emissionTexture)) << 3;
    data.maps[1] = m.normalEncoding;
    for (uint32_t i = 0; i < 2; ++i) {
      const auto &r = m.remap[i];
      data.remap[i] = {r.exponent, r.bias, r.scale, 0};
      data.maps[2 + i] = r.channel | uint32_t(r.invert) << 3 | uint32_t(r.curve) << 4;
    }
    return data;
  }
} // namespace remix::forward
