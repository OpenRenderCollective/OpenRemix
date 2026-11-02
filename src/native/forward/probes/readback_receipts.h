#pragma once
#include "platform/gpu_frames.h"
#include <algorithm>
#include <array>
#include <limits>

namespace remix::forward::probes {
  // Completed ranges collapse into a prefix. Only fenced ranges beyond an
  // outstanding gap need storage, bounded by the owning submission slots.
  // No per-ray bitmap or allocation is required, even for very large captures.
  class ReadbackReceipts {
  public:
    bool consume(uint32_t first, uint32_t count) {
      if (!count || first < m_prefix || count > std::numeric_limits<uint32_t>::max() - first) return false;
      const Range incoming{first, first + count};
      for (uint32_t i = 0; i < m_count; ++i)
        if (incoming.first < m_ranges[i].end && incoming.end > m_ranges[i].first) return false;
      auto ranges = m_ranges;
      ranges[m_count] = incoming;
      std::sort(ranges.begin(), ranges.begin() + m_count + 1,
                [](Range a, Range b) { return a.first < b.first; });
      uint32_t prefix = m_prefix, used = 0;
      for (uint32_t i = 0; i <= m_count; ++i) {
        const auto range = ranges[i];
        if (range.first == prefix) prefix = range.end;
        else if (used && ranges[used - 1].end == range.first) ranges[used - 1].end = range.end;
        else ranges[used++] = range;
      }
      if (used > gpuFrameCount) return false;
      m_prefix = prefix;
      m_count = used;
      m_ranges = ranges;
      return true;
    }
  private:
    struct Range { uint32_t first = 0, end = 0; };
    uint32_t m_prefix = 0, m_count = 0;
    std::array<Range, gpuFrameCount + 1> m_ranges{}; // one temporary insertion slot
  };
}
