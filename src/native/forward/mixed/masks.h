#pragma once
#include "forward/lightmap/layout.h"
#include "forward/mixed/mask_codec.h"
#include <span>
#include <set>

namespace remix::forward::mixed {
  // Scalar visibility is independent of bounded directional SH ownership.
  // Overflow preparation can use bounded-error palettes; primary publication
  // uses lossless encoding after R8 conversion. Constant and binary coverage
  // stay exact. Many chart/light rectangles share indexed storage.
  class Masks {
  public:
    static constexpr uint32_t maximumBytes = 16u << 20; // encoded tables + quantized values
    static constexpr uint32_t maximumPublishedBytes = 64u << 20; // combined primary + overflow publication
    struct Entry {
      uint32_t chart, light, offset, width, height;
      uint32_t encoding = 0;
    };
    bool add(uint32_t chart, uint32_t light, const lightmap::Layout &layout,
             std::span<const float> values, uint64_t wordBudget = ~0u, std::stop_token cancelled = {});
    std::span<const Entry> entries() const { return m_entries; }
    std::span<const uint8_t> bytes() const { return m_bytes; }
    const mask::Costs &candidateCosts() const { return m_candidateCosts; }
    // Validated cache import. Dimensions and identities must agree with the
    // published layout/source; offsets are contiguous, including word padding.
    static Masks restore(std::span<const Entry> entries, std::span<const uint8_t> bytes,
                         const lightmap::Layout &layout, uint32_t lights,
                         uint64_t byteBudget = maximumBytes);
    // Chart-major binary-search tables and packed bytes. All offsets are
    // relative to this tail, appended to the existing occlusion texture.
    std::vector<uint32_t> words(uint32_t charts) const;

  private:
    std::vector<Entry> m_entries;
    std::vector<uint8_t> m_bytes;
    std::set<std::pair<uint32_t, uint32_t>> m_identities;
    mask::Costs m_candidateCosts;
  };
} // namespace remix::forward::mixed
