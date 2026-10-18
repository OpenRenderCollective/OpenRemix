#include "forward/bake/sparse_sh.h"
#include "forward/bake/spherical_harmonics.h"
#include "forward/half.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace remix::forward::bake::sparse {
  namespace {
    constexpr uint32_t tileRecords = tileSide * tileSide;
    void check(bool valid) {
      if (!valid) throw std::invalid_argument("Invalid sparse styled irradiance");
    }
    void cancelled(std::stop_token token) {
      if (token.stop_requested()) throw std::runtime_error("Sparse style encoding superseded");
    }
    uint32_t tiles(uint32_t size) { return (size + tileSide - 1) / tileSide; }
    uint64_t surfaceBlocks(Dimensions d) { return uint64_t(tiles(d.pageSize)) * tiles(d.pageSize) * d.pages; }
    void checkDimensions(Dimensions d) {
      check((d.pages == 0 || d.pageSize > 0) && d.pageSize <= 4096 && d.pages <= 256);
      check(uint64_t(d.pageSize) * d.pageSize * d.pages <= UINT32_MAX);
    }
    using Record = std::array<uint32_t, coefficientWords>;
    Record encode(const Result &value) {
      Record record{};
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        for (uint32_t channel = 0; channel < 3; ++channel) {
          const uint32_t index = coefficient * 3 + channel;
          const uint16_t bits = half::encode(value.irradiance[coefficient][channel]);
          record[index / 2] |= uint32_t(bits) << ((index & 1u) * 16);
        }
      return record;
    }
    bool black(const Record &value) {
      return std::all_of(value.begin(), value.end(), [](uint32_t word) { return (word & 0x7fff7fffu) == 0; });
    }
    uint64_t componentWords(const Component &c) { return entryWords + c.surface.size() + c.probes.size() + c.coefficients.size(); }
    void append(Component &c, std::span<const uint32_t> block, uint64_t availableWords) {
      check(uint64_t(c.coefficients.size()) + block.size() <= UINT32_MAX);
      if (componentWords(c) + block.size() > availableWords)
        throw std::length_error("Styled irradiance exceeds its sparse storage budget");
      c.coefficients.insert(c.coefficients.end(), block.begin(), block.end());
    }
    std::span<const uint32_t> entry(std::span<const uint32_t> words, uint32_t style) {
      if (words.size() < headerWords || words[0] != magic) return {};
      for (uint32_t i = 0; i < words[3]; ++i) {
        const auto e = words.subspan(headerWords + size_t(i) * entryWords, entryWords);
        if (e[0] == style) return e;
      }
      return {};
    }
    Coefficients decode(std::span<const uint32_t> words, std::span<const uint32_t> e, uint32_t record) {
      Coefficients result{};
      if (e.empty() || record == 0) return result;
      const size_t base = e[3] + size_t(record - 1) * coefficientWords;
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        for (uint32_t channel = 0; channel < 3; ++channel) {
          const uint32_t index = coefficient * 3 + channel;
          result[coefficient][channel] = half::decode(uint16_t(words[base + index / 2] >> ((index & 1u) * 16)));
        }
      return result;
    }
  } // namespace
  Builder::Builder(Dimensions d, uint64_t maximumBytes) : m_dimensions(d), m_maximumBytes(maximumBytes) {
    checkDimensions(d);
    check(maximumBytes >= headerWords * 4 && maximumBytes / 4 <= UINT32_MAX);
  }
  uint64_t Builder::bytes() const {
    uint64_t words = headerWords;
    for (const auto &c : m_components) words += componentWords(c);
    return words * 4;
  }
  void Builder::add(uint32_t style, const lightmap::Layout &layout, std::span<const Result> surfaces,
                     std::span<const Result> probeValues, std::stop_token token) {
    check(style > 0 && style < 128 &&
          std::none_of(m_components.begin(), m_components.end(), [&](const auto &c) { return c.style == style; }));
    check(layout.pageSize == m_dimensions.pageSize && layout.pages == m_dimensions.pages &&
          layout.owners.size() == uint64_t(layout.pageSize) * layout.pageSize * layout.pages &&
          surfaces.size() == layout.points.size() && probeValues.size() == m_dimensions.probes);
    cancelled(token);
    const uint64_t available = m_maximumBytes / 4 - bytes() / 4;
    const uint64_t maps = surfaceBlocks(m_dimensions) + m_dimensions.probes;
    if (maps + entryWords > available) throw std::length_error("Styled irradiance lookup exceeds its storage budget");
    Component result;
    result.style = style;
    result.surface.resize(size_t(surfaceBlocks(m_dimensions)));
    result.probes.resize(m_dimensions.probes);
    const uint32_t wide = tiles(layout.pageSize);
    std::array<uint32_t, tileRecords * coefficientWords> block{};
    for (uint32_t tile = 0; tile < result.surface.size(); ++tile) {
      cancelled(token);
      block.fill(0);
      const uint32_t page = tile / (wide * wide), tx = tile % wide, ty = tile / wide % wide;
      bool used = false;
      for (uint32_t y = 0; y < tileSide; ++y) for (uint32_t x = 0; x < tileSide; ++x) {
        const uint32_t px = tx * tileSide + x, py = ty * tileSide + y;
        if (px >= layout.pageSize || py >= layout.pageSize) continue;
        const uint32_t owner = layout.owners[(size_t(page) * layout.pageSize + py) * layout.pageSize + px];
        if (owner == lightmap::unowned) continue;
        check(owner < surfaces.size());
        const auto record = encode(surfaces[owner]);
        used |= !black(record);
        std::copy(record.begin(), record.end(), block.begin() + (y * tileSide + x) * coefficientWords);
      }
      if (!used) continue;
      result.surface[tile] = uint32_t(result.coefficients.size() / coefficientWords) + 1;
      append(result, block, available);
    }
    for (uint32_t index = 0; index < probeValues.size(); ++index) {
      if ((index & 255u) == 0) cancelled(token);
      const auto record = encode(probeValues[index]);
      if (black(record)) continue;
      result.probes[index] = uint32_t(result.coefficients.size() / coefficientWords) + 1;
      append(result, record, available);
    }
    cancelled(token);
    m_components.push_back(std::move(result)); // failed/cancelled additions leave previous components intact
  }
  std::vector<uint32_t> Builder::serialize(std::stop_token token) const {
    cancelled(token);
    std::vector<const Component *> ordered;
    for (const auto &c : m_components) ordered.push_back(&c);
    std::sort(ordered.begin(), ordered.end(), [](auto a, auto b) { return a->style < b->style; });
    std::vector<uint32_t> words(headerWords + ordered.size() * entryWords);
    words.reserve(size_t(bytes() / 4));
    const auto d = m_dimensions;
    const uint32_t header[] = {magic, version, uint32_t(bytes() / 4), uint32_t(ordered.size()),
                               d.pageSize, d.pages, d.probes, tileSide, headerWords, entryWords, coefficientWords, 0};
    std::copy(std::begin(header), std::end(header), words.begin());
    for (size_t i = 0; i < ordered.size(); ++i) {
      cancelled(token);
      const auto &c = *ordered[i];
      const uint32_t surfaceOffset = uint32_t(words.size());
      words.insert(words.end(), c.surface.begin(), c.surface.end());
      const uint32_t probeOffset = uint32_t(words.size());
      words.insert(words.end(), c.probes.begin(), c.probes.end());
      const uint32_t coefficientOffset = uint32_t(words.size());
      words.insert(words.end(), c.coefficients.begin(), c.coefficients.end());
      const uint32_t e[] = {c.style, surfaceOffset, probeOffset, coefficientOffset,
                            uint32_t(c.coefficients.size() / coefficientWords),
                            uint32_t(c.surface.size()), uint32_t(c.probes.size()), 0};
      std::copy(std::begin(e), std::end(e), words.begin() + headerWords + i * entryWords);
    }
    return words;
  }
  void validate(std::span<const uint32_t> words, Dimensions expected,
                  std::span<const uint32_t> styles, uint64_t maximumBytes) {
    checkDimensions(expected);
    check(words.size() >= headerWords && words.size_bytes() <= maximumBytes && words.size() <= UINT32_MAX);
    check(words[0] == magic && words[1] == version && words[2] == words.size() && words[3] == styles.size());
    check(words[4] == expected.pageSize && words[5] == expected.pages && words[6] == expected.probes &&
          words[7] == tileSide && words[8] == headerWords && words[9] == entryWords &&
          words[10] == coefficientWords && words[11] == 0 && styles.size() <= 127);
    uint64_t cursor = headerWords + styles.size() * entryWords;
    check(cursor <= words.size());
    uint32_t previous = 0;
    for (uint32_t i = 0; i < styles.size(); ++i) {
      const auto e = words.subspan(headerWords + size_t(i) * entryWords, entryWords);
      check(e[0] > previous && e[0] < 128 && e[0] == styles[i] && e[7] == 0);
      previous = e[0];
      check(e[5] == surfaceBlocks(expected) && e[6] == expected.probes && e[1] == cursor);
      cursor += e[5];
      check(e[2] == cursor);
      cursor += e[6];
      check(e[3] == cursor);
      cursor += uint64_t(e[4]) * coefficientWords;
      check(cursor <= words.size());
      uint64_t records = 0;
      for (uint32_t tile = 0; tile < e[5]; ++tile) {
        const auto value = words[e[1] + tile];
        if (!value) continue;
        check(value == records + 1 && records + tileRecords <= e[4]);
        records += tileRecords;
      }
      for (uint32_t probe = 0; probe < e[6]; ++probe) {
        const auto value = words[e[2] + probe];
        if (!value) continue;
        check(value == records + 1 && records < e[4]);
        ++records;
      }
      check(records == e[4]);
      for (uint32_t record = 0; record < e[4]; ++record) {
        const size_t offset = e[3] + size_t(record) * coefficientWords;
        for (uint32_t value = 0; value < 27; ++value)
          check(((words[offset + value / 2] >> ((value & 1u) * 16)) & 0x7c00u) != 0x7c00u);
        check((words[offset + 13] & 0xffff0000u) == 0);
      }
    }
    check(cursor == words.size());
  }
  Coefficients probe(std::span<const uint32_t> words, uint32_t style, uint32_t index) {
    const auto e = entry(words, style);
    if (e.empty() || index >= words[6]) return {};
    return decode(words, e, words[e[2] + index]);
  }
  Coefficients surface(std::span<const uint32_t> words, uint32_t style, uint32_t page, uint32_t x, uint32_t y) {
    const auto e = entry(words, style);
    if (e.empty() || page >= words[5] || x >= words[4] || y >= words[4]) return {};
    const uint32_t wide = tiles(words[4]);
    const uint32_t tile = (page * wide + y / tileSide) * wide + x / tileSide;
    const uint32_t base = words[e[1] + tile];
    return decode(words, e, base ? base + (y % tileSide) * tileSide + x % tileSide : 0);
  }
  Vec3 surfaceIrradiance(std::span<const uint32_t> words, std::span<const StyleScale> scales,
                          Vec4 coordinate, Vec3 normal) {
    Vec3 result{};
    if (words.size() < headerWords || coordinate[3] < .5f || words[4] == 0 ||
        coordinate[2] < 0 || coordinate[2] >= words[5]) return result;
    const float x = std::clamp(coordinate[0] * words[4] - .5f, 0.f, float(words[4] - 1));
    const float y = std::clamp(coordinate[1] * words[4] - .5f, 0.f, float(words[4] - 1));
    const uint32_t x0 = uint32_t(x), y0 = uint32_t(y),
                   x1 = std::min(x0 + 1, words[4] - 1), y1 = std::min(y0 + 1, words[4] - 1);
    const auto sh = irradianceBasis(normalized(normal));
    for (const auto &scale : scales) {
      if (scale.status != StyleStatus::Ready || scale.value == 0) continue;
      const auto a = surface(words, scale.style, uint32_t(coordinate[2]), x0, y0);
      const auto b = surface(words, scale.style, uint32_t(coordinate[2]), x1, y0);
      const auto c = surface(words, scale.style, uint32_t(coordinate[2]), x0, y1);
      const auto d = surface(words, scale.style, uint32_t(coordinate[2]), x1, y1);
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        for (uint32_t channel = 0; channel < 3; ++channel)
          result[channel] += scale.value * sh[coefficient] *
              std::lerp(std::lerp(a[coefficient][channel], b[coefficient][channel], x - x0),
                        std::lerp(c[coefficient][channel], d[coefficient][channel], x - x0), y - y0);
    }
    return result; // signed; combine with base before clamping
  }
} // namespace remix::forward::bake::sparse
