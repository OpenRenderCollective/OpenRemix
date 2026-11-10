#include "forward/mixed/mask_codec.h"
#include "forward/mixed/mask_palette.h"
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
        const auto approximate = mask::encodeCompact(source, width, height);
        require(mask::validate(approximate.bytes, width, height, approximate.encoding) == approximate.bytes.size(),
                "Compact masks validate for odd dimensions and partial edge tiles");
        for (uint32_t y = 0; y < height; ++y)
          for (uint32_t x = 0; x < width; ++x) {
            const auto original = source[size_t(y) * width + x];
            const auto decoded = mask::texel(approximate.bytes, width, approximate.encoding, x, y);
            require((decoded == 0) == (original == 0) && (decoded == 255) == (original == 255),
                    "Compact masks preserve endpoint identity at every texel, including padded edges");
            require(std::abs(int(decoded) - original) <= int(mask::maximumPaletteError),
                    "Compact encoding never exceeds its per-texel soft-shadow error limit");
          }
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
  std::vector<uint8_t> fourColors(64);
  constexpr uint8_t paletteValues[] = {0, 1, 254, 255};
  for (uint32_t i = 0; i < fourColors.size(); ++i)
    fourColors[i] = paletteValues[i % 4];
  const auto palette = mask::encodeCompact(fourColors, 8, 8);
  require(palette.encoding == mask::tiled && palette.bytes.size() == 24 &&
              mask::validate(palette.bytes, 8, 8, palette.encoding) == 24,
          "Local four-colour palettes occupy twenty payload bytes plus one descriptor");
  for (uint32_t i = 0; i < fourColors.size(); ++i)
    require(mask::texel(palette.bytes, 8, palette.encoding, i % 8, i / 8) == fourColors[i],
            "A palette preserves all four colours exactly, including faint visibility and nearly clear samples");
  for (uint32_t mode = 1; mode <= 2; ++mode) {
    constexpr uint8_t eightColors[] = {0, 1, 40, 80, 120, 160, 220, 255};
    std::vector<uint8_t> input(64);
    for (uint32_t i = 0; i < 64; ++i)
      input[i] = mode == 1 ? eightColors[i % 8] : uint8_t((i % 16) * 17);
    const auto fitted = mask::encodeCompact(input, 8, 8);
    require(fitted.encoding == mask::tiled && (fitted.bytes[0] & 3) == mode &&
                fitted.bytes.size() == (mode == 1 ? 36 : 52),
            "Difficult gradients promote to eight/sixteen colours instead of exceeding the error bound");
    require(mask::validate(fitted.bytes, 8, 8, fitted.encoding) == fitted.bytes.size(),
            "Variable palette sizes have canonical word-aligned payloads");
    for (uint32_t i = 0; i < 64; ++i)
      require(mask::texel(fitted.bytes, 8, fitted.encoding, i % 8, i / 8) == input[i],
              "Eight/sixteen-colour palettes preserve exact colours across packed-index byte boundaries");
    auto invalidMode = fitted.bytes;
    invalidMode[0] |= 3;
    refused(invalidMode, 8, 8, mask::tiled);
  }
  for (uint32_t pattern = 0; pattern < 4; ++pattern) {
    std::vector<uint8_t> input(256);
    for (uint32_t i = 0; i < input.size(); ++i)
      input[i] = pattern == 0 ? uint8_t(i) : pattern == 1 ? uint8_t(1 + i % 8) :
                 pattern == 2 ? uint8_t(247 + i % 8) : uint8_t((i * 123 + i * i * 17) & 255);
    const auto fitted = mask::encodeCompact(input, 16, 16);
    require(mask::validate(fitted.bytes, 16, 16, fitted.encoding) == fitted.bytes.size(),
            "Fitted palettes have canonical sorted colours and packed payloads");
    for (uint32_t i = 0; i < input.size(); ++i) {
      const auto decoded = mask::texel(fitted.bytes, 16, fitted.encoding, i % 16, i / 16);
      require((decoded == 0) == (input[i] == 0) && (decoded == 255) == (input[i] == 255),
              "Only truly blocked/clear R8 samples decode to those endpoints");
      if (pattern == 1 || pattern == 2)
        require(std::abs(int(decoded) - input[i]) <= 1,
                "Adaptive soft colours retain fine gradients near visibility endpoints");
    }
  }
  auto badPalette = palette.bytes;
  badPalette[4] = 2; // Colours must be sorted.
  refused(badPalette, 8, 8, mask::tiled);
  badPalette = palette.bytes;
  badPalette[5] = 0; // Duplicate colour selected by the existing indices.
  refused(badPalette, 8, 8, mask::tiled);
  badPalette = palette.bytes;
  badPalette.pop_back();
  refused(badPalette, 8, 8, mask::tiled);
  std::printf("Visibility codec: %u tiled / %u raw cases; exact R8 round-trips and malformed payload rejection passed\n",
              tiled, raw);
}
