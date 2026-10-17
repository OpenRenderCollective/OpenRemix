#include "forward/mixed/mask_codec.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace mask = remix::forward::mixed::mask;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  void refused(const std::vector<uint8_t> &bytes, uint32_t width, uint32_t height, uint32_t encoding) {
    bool rejected = false;
    try { mask::validate(bytes, width, height, encoding); }
    catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "Malformed visibility chunks must be rejected");
  }
}
int main() {
  uint32_t tiled = 0, raw = 0;
  for (uint32_t width : {1u, 5u, 8u, 9u, 17u, 64u})
    for (uint32_t height : {1u, 7u, 8u, 13u, 32u})
      for (uint32_t pattern = 0; pattern < 5; ++pattern) {
        std::vector<uint8_t> source(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y)
          for (uint32_t x = 0; x < width; ++x) {
            uint8_t value = 128;
            if (pattern == 1) value = ((x + y) & 1) ? 255 : 0;
            if (pattern == 2) value = uint8_t((x * 123 + y * 29 + x * y * 17) & 255);
            if (pattern == 3) value = x < width / 2 ? 0 : 255;
            if (pattern == 4) value = x < width / 2 ? 255 : uint8_t((x + y) * 11);
            source[size_t(y) * width + x] = value;
          }
        const auto packed = mask::encode(source, width, height);
        tiled += packed.encoding == mask::tiled;
        raw += packed.encoding == mask::raw;
        require(packed.bytes.size() <= ((source.size() + 3) & ~size_t(3)), "Encoding never costs more than raw R8");
        require(mask::validate(packed.bytes, width, height, packed.encoding) == packed.bytes.size(),
                "Validated chunks consume exactly their encoded bytes");
        for (uint32_t y = 0; y < height; ++y)
          for (uint32_t x = 0; x < width; ++x)
            require(mask::texel(packed.bytes, width, packed.encoding, x, y) == source[size_t(y) * width + x],
                    "Visibility tiles round-trip every R8 value without changing edges or gradients");
        auto truncated = packed.bytes;
        truncated.pop_back();
        refused(truncated, width, height, packed.encoding);
      }
  const std::vector<uint8_t> constant(256, 73);
  const auto compact = mask::encode(constant, 16, 16);
  require(compact.encoding == mask::tiled && compact.bytes.size() == 16,
          "Constant tiles store their actual byte rather than forcing visibility to zero or one");
  auto invalid = compact.bytes;
  invalid[3] = 0xc0;
  refused(invalid, 16, 16, mask::tiled);
  invalid = compact.bytes;
  invalid[1] = 1;
  refused(invalid, 16, 16, mask::tiled);
  refused(compact.bytes, 16, 16, 99);
  std::vector<uint8_t> binary(256);
  for (size_t i = 0; i < binary.size(); ++i) binary[i] = i & 1 ? 255 : 0;
  auto bits = mask::encode(binary, 16, 16);
  bits.bytes[0] ^= 4;
  refused(bits.bytes, 16, 16, mask::tiled);
  require(tiled > 20 && raw > 20, "Codec tests exercise both compact and raw fallbacks");
  std::printf("Visibility codec: %u tiled / %u raw cases; exact R8 round-trips and malformed payload rejection passed\n",
              tiled, raw);
}
