#include "forward/mixed/occlusion.h"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace remix::forward::mixed {
  Occlusion::Occlusion(uint32_t charts, uint32_t lights)
      : m_charts(charts), m_lights(lights), m_stride(uint32_t((uint64_t(lights) + 15) / 16)) {
    const uint64_t words = uint64_t(charts) * m_stride;
    if (words > (64u * 1024 * 1024) / sizeof(uint32_t))
      throw std::length_error("Mixed-light occlusion exceeds the 64 MiB metadata budget");
    m_words.resize(size_t(words));
  }
  bool Occlusion::contains(uint32_t chart, uint32_t light) const {
    return state(chart, light) == Visibility::Blocked;
  }
  Occlusion::Occlusion(uint32_t charts, uint32_t lights, std::span<const uint32_t> words)
      : Occlusion(charts, lights) {
    if (words.size() != m_words.size())
      throw std::invalid_argument("Incomplete mixed-light occlusion words");
    for (size_t i = 0; i < words.size(); ++i) {
      const uint32_t word = words[i];
      if ((word & (word >> 1) & 0x55555555u) ||
          (lights % 16 && i % m_stride == m_stride - 1 && word >> ((lights % 16) * 2)))
        throw std::invalid_argument("Invalid mixed-light occlusion state");
      m_count += std::popcount(word & 0x55555555u);
      m_clearCount += std::popcount(word & 0xaaaaaaaau);
      m_words[i] = word;
    }
  }
  Visibility Occlusion::state(uint32_t chart, uint32_t light) const {
    if (chart >= m_charts || light >= m_lights)
      return Visibility::Unknown;
    return Visibility((m_words[size_t(chart) * m_stride + light / 16] >> ((light % 16) * 2)) & 3u);
  }
  void Occlusion::mark(uint32_t chart, uint32_t light, Visibility value) {
    if (chart >= m_charts || light >= m_lights)
      throw std::out_of_range("Invalid mixed-light occlusion identity");
    if (value != Visibility::Blocked && value != Visibility::Clear)
      throw std::invalid_argument("Invalid constant visibility state");
    const auto previous = state(chart, light);
    if (previous == value)
      return;
    if (previous != Visibility::Unknown)
      throw std::logic_error("Conflicting constant visibility states");
    m_words[size_t(chart) * m_stride + light / 16] |= uint32_t(value) << ((light % 16) * 2);
    if (value == Visibility::Blocked)
      ++m_count;
    else
      ++m_clearCount;
  }
  Occlusion::Pixels Occlusion::pixels(uint32_t maximumLayers, std::span<const uint32_t> tail) const {
    constexpr uint64_t side = occlusionPageSide, pageWords = side * side;
    const uint64_t count = 2 + m_words.size() + tail.size();
    if (count > uint64_t(maximumLayers) * pageWords)
      throw std::length_error("Mixed-light occlusion exceeds the device texture limit");
    const auto width = uint32_t(std::min(count, side));
    const auto height = uint32_t(std::min((count + width - 1) / width, side));
    const auto layers = uint32_t((count + pageWords - 1) / pageWords);
    Pixels result{width, height, layers, {}};
    result.words.resize(size_t(width) * height * layers);
    auto store = [&](size_t index, uint32_t value) { result.words[index] = value; };
    store(0, m_stride);
    store(1, m_charts);
    for (size_t i = 0; i < m_words.size(); ++i)
      store(i + 2, m_words[i]);
    for (size_t i = 0; i < tail.size(); ++i)
      store(i + 2 + m_words.size(), tail[i]);
    return result;
  }
} // namespace remix::forward::mixed
