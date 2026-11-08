#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace remix::scene {
  // Set of 64-bit identities rebuilt every frame: open addressing in one
  // array, so inserting allocates nothing once warm and clear() keeps storage.
  class IdSet {
  public:
    // True when id was not yet present, like std::unordered_set::insert().second.
    bool insert(uint64_t id) {
      if (!id)
        return !std::exchange(m_zero, true);
      if ((m_count + 1) * 2 > m_slots.size())
        grow();
      auto &slot = m_slots[locate(id)];
      if (slot)
        return false;
      slot = id;
      ++m_count;
      return true;
    }
    size_t count(uint64_t id) const { return id ? !m_slots.empty() && m_slots[locate(id)] == id : m_zero; }
    size_t size() const { return m_count + m_zero; }
    void clear() {
      if (m_count)
        std::fill(m_slots.begin(), m_slots.end(), 0);
      m_count = 0;
      m_zero = false;
    }

  private:
    size_t locate(uint64_t id) const {
      const size_t mask = m_slots.size() - 1;
      const uint64_t h = id * 0x9E3779B97F4A7C15ull;
      for (size_t slot = size_t(h ^ (h >> 29)) & mask;; slot = (slot + 1) & mask)
        if (m_slots[slot] == id || !m_slots[slot])
          return slot;
    }
    void grow() {
      std::vector<uint64_t> old(std::max<size_t>(64, m_slots.size() * 2), 0);
      old.swap(m_slots);
      for (uint64_t id : old)
        if (id)
          m_slots[locate(id)] = id;
    }
    std::vector<uint64_t> m_slots; // zero marks an empty slot
    size_t m_count = 0;
    bool m_zero = false;
  };
} // namespace remix::scene
