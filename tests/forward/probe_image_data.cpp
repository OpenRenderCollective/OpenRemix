#include "forward/probes/image_data.h"
#include "forward/half.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace p = remix::forward::probes;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  template <class F> void invalid(F action) {
    try { action(); } catch (const std::invalid_argument &) { return; }
    require(false, "Invalid reflection image accepted");
  }
  p::Data fixture(uint32_t count, uint32_t size) {
    p::Data data;
    const uint32_t levels = std::bit_width(size), stride = p::levelOffset(size, levels);
    data.header.parameters = {count, size, levels, stride};
    data.texels.resize(size_t(count) * stride);
    for (uint32_t probe = 0; probe < count; ++probe) {
      data.header.probes[probe].position[0] = float(probe + 1);
      for (uint32_t level = 0; level < levels; ++level) {
        const uint32_t n = size >> level;
        for (uint32_t face = 0; face < 6; ++face)
          for (uint32_t y = 0; y < n; ++y)
            for (uint32_t x = 0; x < n; ++x)
              data.texels[size_t(probe) * stride + p::levelOffset(size, level) + (face*n+y)*n+x] =
                  {float(probe*100 + level*10 + face) + float(x)/8, float(y)/16,
                   .001f, std::numeric_limits<float>::infinity()};
      }
    }
    data.texels.front()[2] = 65000;
    return data;
  }
}
void testProbeImageData() {
  require(p::prepareImage({}).pixels.empty(), "Empty reflection asset has no image payload");
  {
    auto data = fixture(1, 16);
    for (auto &texel : data.texels) texel = {4, 4, 4, 123};
    auto reference = p::captureNormalization(data).coefficients[0];
    require(std::abs(reference[0]) < 1e-6f && std::abs(reference[1]) < 1e-6f &&
                std::abs(reference[2]) < 1e-6f && std::abs(reference[3] - 4) < 1e-5f,
            "Uniform capture normalization retains HDR diffuse radiance in every direction");
    const auto stride = data.header.parameters[3];
    for (uint32_t face = 0; face < 6; ++face)
      for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 16; ++x) {
          const auto n = p::direction(face, (x + .5f) / 8 - 1, (y + .5f) / 8 - 1);
          const float value = 3 + .5f * n[0] - .75f * n[1] + n[2];
          data.texels[(face * 16 + y) * 16 + x] = {value, value, value, 999};
        }
    reference = p::captureNormalization(data).coefficients[0];
    require(std::abs(reference[0] - 1.f/3) < 1e-4f && std::abs(reference[1] + .5f) < 1e-4f &&
                std::abs(reference[2] - 2.f/3) < 1e-4f && std::abs(reference[3] - 3) < 1e-4f,
            "Directional normalization uses world axes and cosine-convolved L1 coefficients");
    require(stride > 6 * 16 * 16, "Fixture retains filtered mips ignored by capture normalization");
    for (auto &texel : data.texels) texel = {0, 0, 0, 999};
    require(p::captureNormalization(data).coefficients[0] == remix::forward::Vec4{},
            "Dark captures retain an explicit zero reference for neutral fallback");
    for (auto &texel : data.texels) texel = {8, 2, 1, 999};
    reference = p::captureNormalization(data).coefficients[0];
    require(std::abs(reference[3] - (8 * .2126f + 2 * .7152f + .0722f)) < 1e-5f,
            "Colored HDR capture normalization measures luminance rather than channel average");
  }
  for (auto [count, size] : {std::pair{2u, 8u}, {64u, 8u}, {1u, 256u}, {1u, 512u}}) {
    const auto data = fixture(count, size);
    const auto image = p::prepareImage(data);
    require(std::memcmp(&image.header, &data.header, sizeof(p::Header)) == 0,
            "Image upload preserves every independent projection record");
    require(image.pixels.size() * sizeof(uint16_t) == data.texels.size() * sizeof(remix::forward::Vec4) / 2,
            "RGBA16F upload halves radiance storage without dropping captures or mips");
    const auto stride = data.header.parameters[3];
    for (uint32_t level = 0; level < image.levels.size(); ++level) {
      const uint32_t n = size >> level;
      const auto &mip = image.levels[level];
      require(mip.faceSize == n && mip.byteOffset % 8 == 0, "Mip copies have aligned offsets and exact extents");
      for (uint32_t probe = 0; probe < count; ++probe)
        for (uint32_t pixel = 0; pixel < 6*n*n; ++pixel) {
          const auto &original = data.texels[size_t(probe)*stride + p::levelOffset(size, level) + pixel];
          const auto offset = mip.byteOffset / 2 + (size_t(probe)*6*n*n + pixel)*4;
          for (uint32_t c = 0; c < 3; ++c) {
            const float actual = remix::forward::half::decode(image.pixels[offset+c]);
            require(std::abs(actual-original[c]) <= std::max(1e-7f, original[c]*.001f),
                    "Every probe/face/mip texel survives image repacking within half precision");
          }
          require(image.pixels[offset+3] == 0, "Unused geometry distance is omitted from sampled radiance");
        }
    }
  }
  {
    auto data = fixture(3, 8);
    data.header.probes[0].minimum[3] = -1;
    data.header.probes[1].minimum[3] = data.header.probes[2].minimum[3] = 7;
    const auto original = p::serialize(data);
    const auto image = p::prepareImage(data);
    const uint32_t order[]{1, 2, 0};
    const auto canonicalNormalization = p::captureNormalization(data);
    require(p::serialize(data) == original, "Priority upload sorting preserves canonical cache and reuse data");
    for (uint32_t layer = 0; layer < 3; ++layer) {
      require(image.normalization.coefficients[layer] == canonicalNormalization.coefficients[order[layer]],
              "Normalization sorts with radiance and stable equal-priority projection metadata");
      require(std::memcmp(&image.header.probes[layer], &data.header.probes[order[layer]], sizeof(p::Record)) == 0,
              "Projection metadata sorts with image layers and retains equal-priority order");
      for (uint32_t level = 0; level < image.levels.size(); ++level) {
        const auto &mip = image.levels[level];
        const uint32_t values = 6 * mip.faceSize * mip.faceSize;
        for (uint32_t pixel = 0; pixel < values; ++pixel) {
          const auto &source = data.texels[size_t(order[layer])*data.header.parameters[3] +
                                           p::levelOffset(8, level) + pixel];
          const size_t offset = mip.byteOffset / 2 + (size_t(layer)*values + pixel)*4;
          for (uint32_t c = 0; c < 3; ++c)
            require(image.pixels[offset+c] == remix::forward::half::encode(source[c]),
                    "Priority sorting moves every face and roughness mip with its projection record");
        }
      }
    }
    data.header.probes[1].minimum[3] = std::numeric_limits<float>::quiet_NaN();
    invalid([&] { p::prepareImage(data); });
  }
  auto bad = fixture(1, 8);
  bad.texels.pop_back();
  invalid([&] { p::prepareImage(bad); });
  bad = fixture(1, 8);
  bad.texels[0][0] = std::numeric_limits<float>::quiet_NaN();
  invalid([&] { p::prepareImage(bad); });
  bad.texels[0][0] = 65001;
  invalid([&] { p::prepareImage(bad); });
  bad = fixture(1, 8);
  bad.header.parameters[0] = 0;
  invalid([&] { p::prepareImage(bad); });
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try { p::prepareImage({}, stop.get_token()); } catch (const std::runtime_error &) { cancelled = true; }
  require(cancelled, "Image preparation honours cancellation before allocating");
  std::puts("Reflection image layout, HDR precision, 64 layers and 256/512 faces passed");
}
