#pragma once
#include "forward/mixed/data.h"

namespace remix::forward::mixed {
  // Immutable ownership plus one prefix per receiver replaces the expanded
  // receiver/light list. Batches preserve point order and explicit output tags.
  class DirectWork {
  public:
    struct Owner { uint32_t light, output; };
    DirectWork(const Plan &plan, const lightmap::Layout &layout, std::stop_token cancelled = {});
    // Chart offsets delimit arbitrary owner lists. Output tags survive batching;
    // a collector may use them for legacy channels or sparse field identities.
    DirectWork(uint32_t lightCount, std::span<const uint32_t> chartOffsets,
               std::span<const Owner> owners, const lightmap::Layout &layout,
               std::stop_token cancelled = {});
    uint32_t size() const { return m_count; }
    bool empty() const { return !m_count; }
    uint64_t memoryBytes() const {
      return m_receivers.capacity() * sizeof(Receiver) + m_owners.capacity() * sizeof(Owner) +
             m_chartOffsets.capacity() * sizeof(uint32_t);
    }
    std::vector<Task> batch(uint32_t first, uint32_t count) const;

  private:
    struct Receiver { uint32_t first, chart; };
    std::vector<Receiver> m_receivers; // includes the terminal task-count sentinel
    std::vector<uint32_t> m_chartOffsets;
    std::vector<Owner> m_owners;
    uint32_t m_count = 0;
    void index(uint32_t lightCount, const lightmap::Layout &layout, std::stop_token cancelled);
  };
} // namespace remix::forward::mixed
