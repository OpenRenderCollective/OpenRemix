#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace remix::scene {
  // Incremental xxHash64, matching fastHash without joining input chunks.
  // The existing optimized one-shot implementation remains unchanged.
  class HashStream {
  public:
    explicit HashStream(uint64_t seed = 0)
        : m_seed(seed), m_lanes{seed + p1 + p2, seed + p2, seed, seed - p1} {}
    void append(const void *data, size_t size) {
      if (!size) return;
      auto bytes = static_cast<const uint8_t *>(data);
      m_size += size;
      if (m_pending) {
        const auto count = std::min(size, m_tail.size() - m_pending);
        std::memcpy(m_tail.data() + m_pending, bytes, count);
        m_pending += count;
        bytes += count;
        size -= count;
        if (m_pending < m_tail.size()) return;
        block(m_tail.data());
        m_pending = 0;
      }
      while (size >= m_tail.size()) {
        block(bytes);
        bytes += m_tail.size();
        size -= m_tail.size();
      }
      if (size) std::memcpy(m_tail.data(), bytes, size);
      m_pending = size;
    }
    uint64_t digest() const {
      uint64_t hash = m_seed + p5;
      if (m_size >= 32) {
        hash = std::rotl(m_lanes[0], 1) + std::rotl(m_lanes[1], 7) +
               std::rotl(m_lanes[2], 12) + std::rotl(m_lanes[3], 18);
        for (const auto lane : m_lanes) hash = (hash ^ round(0, lane)) * p1 + p4;
      }
      hash += m_size;
      const auto *bytes = m_tail.data();
      size_t left = m_pending;
      while (left >= 8) {
        hash = std::rotl(hash ^ round(0, read64(bytes)), 27) * p1 + p4;
        bytes += 8;
        left -= 8;
      }
      if (left >= 4) {
        uint32_t value;
        std::memcpy(&value, bytes, sizeof(value));
        hash = std::rotl(hash ^ (uint64_t(value) * p1), 23) * p2 + p3;
        bytes += 4;
        left -= 4;
      }
      while (left--) hash = std::rotl(hash ^ (uint64_t(*bytes++) * p5), 11) * p1;
      hash ^= hash >> 33;
      hash *= p2;
      hash ^= hash >> 29;
      hash *= p3;
      return hash ^ (hash >> 32);
    }
  private:
    static constexpr uint64_t p1 = 0x9E3779B185EBCA87ull, p2 = 0xC2B2AE3D27D4EB4Full,
        p3 = 0x165667B19E3779F9ull, p4 = 0x85EBCA77C2B2AE63ull, p5 = 0x27D4EB2F165667C5ull;
    static uint64_t read64(const uint8_t *bytes) {
      uint64_t value;
      std::memcpy(&value, bytes, sizeof(value));
      return value;
    }
    static uint64_t round(uint64_t accumulator, uint64_t value) {
      return std::rotl(accumulator + value * p2, 31) * p1;
    }
    void block(const uint8_t *bytes) {
      for (size_t lane = 0; lane < m_lanes.size(); ++lane)
        m_lanes[lane] = round(m_lanes[lane], read64(bytes + lane * 8));
    }
    uint64_t m_seed, m_size = 0;
    std::array<uint64_t, 4> m_lanes;
    std::array<uint8_t, 32> m_tail{};
    size_t m_pending = 0;
  };
}
