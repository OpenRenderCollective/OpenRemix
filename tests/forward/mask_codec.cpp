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
  mask::Costs costs;
  const std::vector<uint8_t> smallConstant(64, 73);
  costs.observe(smallConstant, 8, 8, 4);
  require(costs.candidates == 1 && costs.texels == 64 && costs.soft == 64 && costs.rawBytes == 64 &&
              costs.losslessBytes == 4 && costs.twoBitBytes == 4 && costs.fourBitBytes == 4 &&
              costs.tileCardinality[1] == 1 && costs.maximumError[0] == 12,
          "Cost audit accounts for constant tiles and endpoint-preserving quantization error");
  costs = {};
  const std::span binaryTile(binary.data(), 64);
  costs.observe(binaryTile, 8, 8, 12);
  require(costs.blocked == 32 && costs.clear == 32 && costs.soft == 0 &&
              costs.twoBitBytes == 12 && costs.fourBitBytes == 12 && costs.tileCardinality[2] == 1 &&
              costs.absoluteError[0] == 0 && costs.absoluteError[1] == 0,
          "Hard occlusion endpoints cost eight tile payload bytes without quantization error");
  std::vector<uint8_t> ramp(256);
  for (uint32_t i = 0; i < ramp.size(); ++i)
    ramp[i] = uint8_t(i);
  costs = {};
  costs.observe(ramp, 16, 16, 256);
  require(costs.blocked == 1 && costs.clear == 1 && costs.soft == 254 &&
              costs.twoBitBytes == 48 && costs.fourBitBytes == 128 && costs.tileCardinality[64] == 4 &&
              costs.maximumError[0] == 84 && costs.maximumError[1] == 16,
          "Full-range cost estimates include raw packing and expose soft-shadow error instead of rounding endpoints away");
  bool cancelled = false;
  std::stop_source stop;
  stop.request_stop();
  try { costs.observe(ramp, 16, 16, 256, stop.get_token()); }
  catch (const std::runtime_error &) { cancelled = true; }
  require(cancelled, "Candidate-cost diagnostics honor bake cancellation");
  std::printf("Visibility codec: %u tiled / %u raw cases; exact R8 round-trips and malformed payload rejection passed\n",
              tiled, raw);
}
