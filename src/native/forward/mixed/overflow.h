#pragma once
#include "forward/mixed/data.h"

namespace remix::forward::mixed {
  // Classify remaining lights without overwriting the four filtered channels.
  // One light per chart is streamed per round; only chart extrema are retained.
  // Partial results remain Unknown and require the ordinary shadow fallback.
  class OverflowAudit {
  public:
    bool nextRound(Plan &plan, const lightmap::Layout &layout, std::stop_token cancelled = {});
    std::span<const Task> tasks() const { return m_tasks; }
    void observe(uint32_t task, float visibility, const lightmap::Layout &layout);

  private:
    struct Range {
      uint32_t light = none;
      float minimum = 1, maximum = 0;
      bool sampled = false;
    };
    std::vector<Range> m_ranges;
    std::vector<Task> m_tasks;
    std::vector<uint8_t> m_seen;
    uint32_t m_received = 0;
  };
}
