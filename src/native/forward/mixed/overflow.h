#pragma once
#include "forward/mixed/data.h"

namespace remix::forward::mixed {
  // Classify remaining lights without overwriting the four filtered channels.
  // One light per chart is streamed per round; only chart extrema are retained.
  // Partial results retain scalar masks within an explicit memory budget.
  class OverflowAudit {
  public:
    explicit OverflowAudit(uint64_t wordBudget = ~0u) : m_wordBudget(wordBudget) {}
    bool nextRound(Plan &plan, const lightmap::Layout &layout, std::stop_token cancelled = {});
    std::span<const Task> tasks() const { return m_tasks; }
    void observe(uint32_t task, float visibility, const lightmap::Layout &layout);
    uint64_t completedWork() const { return m_completedWork; }
    uint64_t totalWork() const { return m_totalWork; }

  private:
    struct Range {
      uint32_t light = none;
      float minimum = 1, maximum = 0;
      bool sampled = false;
    };
    std::vector<Range> m_ranges;
    std::vector<Task> m_tasks;
    std::vector<uint8_t> m_seen;
    std::vector<float> m_values;
    uint32_t m_received = 0;
    uint64_t m_wordBudget;
    uint64_t m_completedWork = 0, m_totalWork = 0;
    bool m_initialized = false;
    std::vector<uint8_t> m_receivers;
    // Retained receivers and atlas donors, grouped once rather than scanning
    // the entire map again for every overflow round.
    std::vector<std::vector<uint32_t>> m_chartPoints;
  };
}
