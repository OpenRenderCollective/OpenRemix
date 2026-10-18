#include "forward/bake/probe_visibility.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    struct Encoding {
      uint64_t bytes;
      uint32_t rowWords = 0;
      std::vector<uint32_t> slots;
      bool ranked = false;
    };
    Encoding encoding(uint32_t probes, uint32_t lights, std::span<const ProbeVisibility::Entry> entries,
                      uint64_t storageBytes) {
      if (lights >= (1u << 24))
        throw std::length_error("Probe visibility exceeds captured light index range");
      const uint64_t availability = (uint64_t(lights) + 31) / 32;
      Encoding fallback{(16 + availability * 4 + storageBytes + 15) & ~uint64_t(15)};
      if (uint64_t(lights) * 4 > ProbeVisibility::maximumDirectoryBytes)
        return fallback;
      std::vector<uint32_t> slots(lights, UINT32_MAX);
      for (const auto &entry : entries)
        slots[entry.light] = 0;
      uint32_t count = 0;
      for (auto &slot : slots)
        if (slot != UINT32_MAX)
          slot = count++;
      const uint32_t rowWords = (count + 31) / 32;
      const uint64_t directory = (uint64_t(probes) * rowWords * 2 + lights) * sizeof(uint32_t);
      if (directory > ProbeVisibility::maximumDirectoryBytes)
        return fallback;
      // Ranked rows omit the redundant light index beside each scalar mask.
      const uint64_t words =
          5 + uint64_t(lights) + uint64_t(probes) * rowWords * 2 + probes + 1 + entries.size();
      return {(words * 4 + 15) & ~uint64_t(15), rowWords, std::move(slots), true};
    }
  } // namespace
  uint64_t ProbeVisibility::gpuBytes() const {
    const uint64_t directBytes = m_direct.size() * sizeof(Direct);
    const auto scalar = encoding(uint32_t(m_offsets.size() - 1), m_lights, m_entries, bytes() - directBytes).bytes;
    return m_direct.empty() ? scalar : (16 + scalar + directBytes + 15) & ~uint64_t(15);
  }
  std::vector<uint32_t> ProbeVisibility::words() const {
    auto masks = maskWords();
    if (m_direct.empty()) return masks;
    // Version five wraps the unchanged scalar encoding and appends unit direct
    // SH in exactly the entry order used by its ranked/sorted mask indices.
    std::vector<uint32_t> result{0x56504c52u, 5u, uint32_t(m_offsets.size() - 1), uint32_t(masks.size())};
    result.reserve(size_t(gpuBytes() / sizeof(uint32_t)));
    result.insert(result.end(), masks.begin(), masks.end());
    for (const auto &value : m_direct)
      for (float coefficient : value) {
        if (!std::isfinite(coefficient))
          throw std::invalid_argument("Non-finite directional probe coefficient");
        result.push_back(std::bit_cast<uint32_t>(coefficient));
      }
    result.resize(size_t(gpuBytes() / sizeof(uint32_t)));
    return result;
  }
  std::vector<uint32_t> ProbeVisibility::maskWords() const {
    validate(uint32_t(m_offsets.size() - 1), m_lights, m_offsets, m_entries, maximumBytes);
    const uint32_t probes = uint32_t(m_offsets.size() - 1);
    const auto plan = encoding(probes, m_lights, m_entries, bytes() - m_direct.size() * sizeof(Direct));
    if (!plan.ranked) {
      const uint32_t availability = (m_lights + 31) / 32;
      std::vector<uint32_t> result{0x56504c52u, 2u, probes, availability};
      result.resize(4 + availability, 0);
      for (const auto &entry : m_entries)
        result[4 + entry.light / 32] |= 1u << (entry.light % 32);
      result.insert(result.end(), m_offsets.begin(), m_offsets.end());
      for (const auto &entry : m_entries) {
        result.push_back(entry.light);
        result.push_back(std::bit_cast<uint32_t>(entry.visibility));
      }
      result.resize(size_t(plan.bytes / 4));
      return result;
    }
    std::vector<uint32_t> result(size_t(plan.bytes / 4), 0);
    result[0] = 0x56504c52u;
    result[1] = 3;
    result[2] = probes;
    result[3] = m_lights;
    std::copy(plan.slots.begin(), plan.slots.end(), result.begin() + 4);
    result[4 + m_lights] = plan.rowWords;
    const uint32_t rows = 5 + m_lights, offsets = rows + probes * plan.rowWords * 2;
    const uint32_t values = offsets + probes + 1;
    std::copy(m_offsets.begin(), m_offsets.end(), result.begin() + offsets);
    for (uint32_t probe = 0; probe < probes; ++probe) {
      for (uint32_t i = m_offsets[probe]; i < m_offsets[probe + 1]; ++i) {
        const auto &entry = m_entries[i];
        const uint32_t slot = plan.slots[entry.light];
        result[rows + (probe * plan.rowWords + slot / 32) * 2] |= 1u << (slot % 32);
        result[values + i] = std::bit_cast<uint32_t>(entry.visibility);
      }
      uint32_t prefix = 0;
      for (uint32_t word = 0; word < plan.rowWords; ++word) {
        const uint32_t index = rows + (probe * plan.rowWords + word) * 2;
        result[index + 1] = prefix;
        prefix += uint32_t(std::popcount(result[index]));
      }
    }
    return result;
  }
} // namespace remix::forward::bake
