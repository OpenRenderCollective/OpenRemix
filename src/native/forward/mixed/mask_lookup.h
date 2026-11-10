#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace remix::forward::mixed {
  // Optional GPU-only directory. Records and visibility payloads retain their
  // original offsets; zero denotes a missing light. The cache stays compact.
  // The budget counts R32_UINT texture bytes, not just the uint32_t CPU words.
  inline void indexMaskRecords(std::vector<uint32_t> &words, uint32_t charts,
                               uint64_t maximumExtraBytes = 8ull << 20) {
    if (words.empty()) return;
    constexpr uint32_t indexed = 0x80000000u;
    uint64_t extraWords = 0;
    for (uint32_t chart = 0; chart < charts; ++chart) {
      const uint32_t first = words[chart * 2], count = words[chart * 2 + 1];
      if (count < 8 || (count & indexed)) continue;
      const uint64_t span = uint64_t(words[first + (count - 1) * 4]) + 1;
      if (span >= indexed) return;
      extraWords += span;
    }
    if (extraWords > maximumExtraBytes / 4 || words.size() + extraWords >= indexed)
      return; // Atomic fallback: retain every original binary-search table.
    words.reserve(words.size() + std::size_t(extraWords));
    for (uint32_t chart = 0; chart < charts; ++chart) {
      const uint32_t first = words[chart * 2], count = words[chart * 2 + 1];
      if (count < 8 || (count & indexed)) continue;
      const uint32_t span = words[first + (count - 1) * 4] + 1;
      const uint32_t directory = uint32_t(words.size());
      words.resize(words.size() + span, 0);
      for (uint32_t entry = 0; entry < count; ++entry) {
        const uint32_t record = first + entry * 4;
        words[directory + words[record]] = record;
      }
      words[chart * 2] = directory;
      words[chart * 2 + 1] = indexed | span;
    }
  }
}
