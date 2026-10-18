#pragma once
#include "forward/mixed/data.h"

namespace remix::forward::mixed {
  // Immutable ownership plus one prefix per receiver replaces the expanded
  // receiver/light list. Batches preserve point order and sparse channel IDs.
  class DirectWork {
  public:
    DirectWork(const Plan &plan, const lightmap::Layout &layout, std::stop_token cancelled = {});
    uint32_t size() const { return m_count; }
    bool empty() const { return !m_count; }
    uint64_t memoryBytes() const {
      return m_receivers.capacity() * sizeof(Receiver) + m_owners.capacity() * sizeof(bake::UInt4);
    }
    std::vector<Task> batch(uint32_t first, uint32_t count) const;

  private:
    struct Receiver { uint32_t first, chart; };
    std::vector<Receiver> m_receivers; // includes the terminal task-count sentinel
    std::vector<bake::UInt4> m_owners;
    uint32_t m_count = 0;
  };
} // namespace remix::forward::mixed
