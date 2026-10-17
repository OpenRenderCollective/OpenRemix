#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace remix::forward::mixed {
  enum class Visibility : uint32_t { Unknown, Blocked, Clear };
  // Two bits per chart/light pair. Only exactly constant sampled visibility is
  // recorded; partial visibility stays in filtered maps.
  class Occlusion {
  public:
    Occlusion() = default;
    Occlusion(uint32_t charts, uint32_t lights);
    Occlusion(uint32_t charts, uint32_t lights, std::span<const uint32_t> words);
    std::span<const uint32_t> words() const { return m_words; }
    void mark(uint32_t chart, uint32_t light, Visibility value = Visibility::Blocked);
    Visibility state(uint32_t chart, uint32_t light) const;
    bool contains(uint32_t chart, uint32_t light) const;
    uint32_t count() const { return m_count; }
    uint32_t clearCount() const { return m_clearCount; }
    uint32_t wordsPerChart() const { return m_stride; }
    uint32_t chartCount() const { return m_charts; }
    struct Pixels {
      uint32_t width, height;
      std::vector<uint16_t> rgba;
    };
    // Header words (stride, chart count), followed by chart-major bitsets.
    // Each logical byte is an exact integer in an RGBA16F component, using
    // the format already required for HDR without normalized-byte decoding.
    Pixels pixels(uint32_t maximumDimension) const;

  private:
    uint32_t m_charts = 0, m_lights = 0, m_stride = 0, m_count = 0, m_clearCount = 0;
    std::vector<uint32_t> m_words;
  };
} // namespace remix::forward::mixed
