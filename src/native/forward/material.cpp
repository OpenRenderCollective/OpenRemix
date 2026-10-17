#include "forward/material.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace remix::forward {
  static float linear(float c) {
    return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f);
  }
  Matrix normalMatrix(const Matrix &model) {
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
    const uint32_t frame = uint32_t(std::fmod(seconds * m.fps, uint32_t(m.rows) * m.columns));
    data.uv = {1.f / m.columns, 1.f / m.rows, float(frame % m.columns) / m.columns,
               float(frame / m.columns) / m.rows};
    data.pbr = {m.roughness, m.metallic, m.normalTexture && m.normalTexture->pixels.twoChannel ? 1.f : 0.f,
                acceptsTerrainLayer(draw) ? 1.f : 0.f};
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
    data.factor = {linear(float((b.factor >> 16) & 255) / 255), linear(float((b.factor >> 8) & 255) / 255),
                   linear(float(b.factor & 255) / 255), float(b.factor >> 24) / 255};
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
