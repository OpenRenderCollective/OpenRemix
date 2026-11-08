#include "forward/mixed/directional_field.h"
#include <algorithm>
#include <stdexcept>
#include <optional>

namespace remix::forward::mixed::directional {
  namespace {
    constexpr uint32_t tileFields = tileSide * tileSide, offsetMask = 0x3fffffffu;
    void require(bool valid) {
      if (!valid) throw std::invalid_argument("Invalid directional surface field");
    }
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Directional surface encoding superseded");
    }
    size_t fieldCount(uint32_t width, uint32_t height) {
      require(width > 0 && height > 0 && width <= 4096 && height <= 4096);
      return size_t(width) * height;
    }
    size_t wordCount(size_t fields) { return (fields * 9 + 1) / 2; }
    uint32_t columns(uint32_t width, uint32_t side) { return (width + side - 1) / side; }
    uint32_t packPair(uint16_t low, uint16_t high) {
      return uint32_t(low) | (uint32_t(high) << 16);
    }
    void append(std::vector<uint32_t> &words, std::span<const Field> fields) {
      const auto first = words.size();
      words.resize(first + wordCount(fields.size()));
      auto output = words.begin() + first;
      for (size_t i = 0; i < fields.size(); i += 2) {
        const auto &a = fields[i];
        for (size_t c = 0; c + 1 < a.size(); c += 2)
          *output++ = packPair(a[c], a[c + 1]);
        if (i + 1 == fields.size()) {
          *output = a.back(); // The final high half is zero padding.
          break;
        }
        // Nine coefficients put the field boundary inside this word.
        const auto &b = fields[i + 1];
        *output++ = packPair(a.back(), b.front());
        for (size_t c = 1; c < b.size(); c += 2)
          *output++ = packPair(b[c], b[c + 1]);
      }
    }
    void validateFields(std::span<const uint32_t> words, size_t first, size_t count) {
      const size_t size = wordCount(count);
      require(first <= words.size() && size <= words.size() - first);
      constexpr uint32_t lowExponent = 0x7c00u, highExponent = lowExponent << 16;
      const auto fieldWords = words.subspan(first, size);
      for (uint32_t word : fieldWords)
        require((word & lowExponent) != lowExponent && (word & highExponent) != highExponent);
      // Each field has nine halves, so an odd field count leaves one pad half.
      if (count & 1u) require((fieldWords.back() >> 16) == 0);
    }
    Encoded rawImage(std::span<const Field> fields, std::stop_token cancelled) {
      Encoded result;
      result.words.reserve(wordCount(fields.size()));
      // Even field counts keep adjacent batches half-word aligned.
      for (size_t first = 0; first < fields.size(); first += 1024) {
        check(cancelled);
        append(result.words, fields.subspan(first, std::min(size_t(1024), fields.size() - first)));
      }
      return result;
    }
    Field read(std::span<const uint32_t> words, size_t first, size_t field) {
      Field result;
      const size_t firstHalf = field * result.size();
      auto input = words.begin() + first + firstHalf / 2;
      size_t coefficient = 0;
      if (firstHalf & 1u)
        result[coefficient++] = uint16_t(*input++ >> 16);
      for (; coefficient + 1 < result.size(); coefficient += 2) {
        const uint32_t pair = *input++;
        result[coefficient] = uint16_t(pair);
        result[coefficient + 1] = uint16_t(pair >> 16);
      }
      if (coefficient < result.size()) result[coefficient] = uint16_t(*input);
      return result;
    }
    std::optional<Encoded> encodeTiles(std::span<const Field> fields, uint32_t width, uint32_t height,
        std::stop_token cancelled, uint64_t maximumWords, uint32_t side, uint32_t encoding) {
      const uint32_t tilesX = columns(width, side), tilesY = columns(height, side);
      const size_t descriptors = size_t(tilesX) * tilesY;
      if (descriptors > maximumWords) return std::nullopt;
      Encoded result{encoding, std::vector<uint32_t>(descriptors)};
      std::array<Field, tileFields> block;
      const uint32_t blockFields = side * side;
      for (uint32_t ty = 0; ty < tilesY; ++ty) {
        check(cancelled);
        for (uint32_t tx = 0; tx < tilesX; ++tx) {
          bool constant = true;
          for (uint32_t y = 0; y < side; ++y)
            for (uint32_t x = 0; x < side; ++x) {
              const auto px = std::min(tx * side + x, width - 1);
              const auto py = std::min(ty * side + y, height - 1);
              const auto &field = fields[size_t(py) * width + px];
              block[y * side + x] = field;
              constant = constant && field == block[0];
            }
          // Only a constant tile can be entirely zero.
          if (constant && block[0] == Field{}) continue;
          const size_t next = result.words.size() + wordCount(constant ? 1 : blockFields);
          if (next > maximumWords) return std::nullopt;
          require(result.words.size() <= offsetMask);
          result.words[ty * tilesX + tx] = uint32_t(result.words.size()) | (constant ? 1u : 2u) << 30;
          append(result.words, std::span(block).first(constant ? 1 : blockFields));
        }
      }
      check(cancelled);
      return result;
    }
  }
  Encoded encode(std::span<const Field> fields, uint32_t width, uint32_t height, std::stop_token cancelled,
                 uint64_t maximumWords) {
    require(fields.size() == fieldCount(width, height));
    check(cancelled);
    for (size_t i = 0; i < fields.size(); ++i) {
      if ((i & 1023u) == 0) check(cancelled);
      for (auto coefficient : fields[i]) require((coefficient & 0x7c00u) != 0x7c00u);
    }
    // Tiled candidates must beat raw storage. Materialize the raw payload
    // only if neither tile size wins, and prefer the larger tiles on ties.
    const auto rawWords = wordCount(fields.size());
    const auto tiledLimit = std::min<uint64_t>(maximumWords, rawWords - 1);
    auto best = encodeTiles(fields, width, height, cancelled, tiledLimit, tileSide, tiled);
    const auto limit = best ? best->words.size() - 1 : tiledLimit;
    if (auto compact = encodeTiles(fields, width, height, cancelled, limit, 4, tiledSmall))
      best = std::move(compact);
    if (best) return std::move(*best);
    if (rawWords <= maximumWords) return rawImage(fields, cancelled);
    throw std::length_error("Directional field exceeds its encoding budget");
  }
  size_t validate(std::span<const uint32_t> words, uint32_t width, uint32_t height, uint32_t encoding) {
    const size_t count = fieldCount(width, height);
    if (encoding == raw) {
      validateFields(words, 0, count);
      return wordCount(count);
    }
    require(encoding == tiled || encoding == tiledSmall);
    const uint32_t side = encoding == tiledSmall ? 4 : tileSide;
    const size_t descriptors = size_t(columns(width, side)) * columns(height, side);
    require(descriptors <= words.size());
    size_t cursor = descriptors;
    for (size_t tile = 0; tile < descriptors; ++tile) {
      const auto descriptor = words[tile], mode = descriptor >> 30;
      if (mode == 0) {
        require(descriptor == 0);
        continue;
      }
      require((mode == 1 || mode == 2) && (descriptor & offsetMask) == cursor);
      const size_t fields = mode == 1 ? 1 : side * side;
      validateFields(words, cursor, fields);
      cursor += wordCount(fields);
    }
    return cursor;
  }
  Field texel(std::span<const uint32_t> words, uint32_t width, uint32_t encoding, uint32_t x, uint32_t y) {
    if (encoding == raw) return read(words, 0, size_t(y) * width + x);
    const uint32_t side = encoding == tiledSmall ? 4 : tileSide;
    const auto descriptor = words[size_t(y / side) * columns(width, side) + x / side];
    if (descriptor == 0) return {};
    const auto mode = descriptor >> 30, offset = descriptor & offsetMask;
    return read(words, offset, mode == 1 ? 0 : (y % side) * side + x % side);
  }
}
