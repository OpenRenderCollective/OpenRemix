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
