#include "forward/bake/probe_visibility.h"
#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace remix::forward::bake {
  namespace {
    size_t nonzeroFieldCount(std::span<const UnitDirect> fields) {
      return size_t(std::count_if(fields.begin(), fields.end(),
          [](const auto &field) { return !zeroUnitDirect(field); }));
    }
    struct Encoding {
      uint64_t bytes;
      uint32_t rowWords = 0;
      std::vector<uint32_t> slots;
      bool ranked = false;
    };
    Encoding encoding(uint32_t probes, uint32_t lights, std::span<const ProbeVisibility::Entry> entries,
                      uint64_t storageBytes, bool buildSlots) {
      if (lights >= (1u << 24))
        throw std::length_error("Probe visibility exceeds captured light index range");
      const uint64_t availability = (uint64_t(lights) + 31) / 32;
      Encoding fallback{(16 + availability * 4 + storageBytes + 15) & ~uint64_t(15)};
      if (uint64_t(lights) * 4 > ProbeVisibility::maximumDirectoryBytes)
        return fallback;
      std::vector<uint32_t> present(size_t(availability), 0);
      for (const auto &entry : entries)
        present[entry.light / 32] |= 1u << (entry.light % 32);
      uint32_t count = 0;
      for (uint32_t word : present) count += uint32_t(std::popcount(word));
      const uint32_t rowWords = (count + 31) / 32;
      const uint64_t directory = (uint64_t(probes) * rowWords * 2 + lights) * sizeof(uint32_t);
      if (directory > ProbeVisibility::maximumDirectoryBytes)
        return fallback;
      std::vector<uint32_t> slots;
      if (buildSlots) {
        slots.resize(lights, UINT32_MAX);
        uint32_t rank = 0;
        for (uint32_t block = 0; block < present.size(); ++block)
          for (uint32_t bits = present[block]; bits; bits &= bits - 1)
            slots[block * 32 + std::countr_zero(bits)] = rank++;
      }
      // Ranked rows omit the redundant light index beside each scalar mask.
      const uint64_t words =
          5 + uint64_t(lights) + uint64_t(probes) * rowWords * 2 + probes + 1 + entries.size();
      return {(words * 4 + 15) & ~uint64_t(15), rowWords, std::move(slots), true};
    }
    // The destination is already zero-initialized, including alignment padding.
    void writeMasks(std::span<uint32_t> result, const Encoding &plan, uint32_t lights,
                    std::span<const uint32_t> probeOffsets, std::span<const ProbeVisibility::Entry> entries) {
      const uint32_t probes = uint32_t(probeOffsets.size() - 1);
      if (!plan.ranked) {
        const uint32_t availability = (lights + 31) / 32;
        const uint32_t offsets = 4 + availability;
        result[0] = 0x56504c52u;
        result[1] = 2;
        result[2] = probes;
        result[3] = availability;
        std::copy(probeOffsets.begin(), probeOffsets.end(), result.begin() + offsets);
        size_t value = offsets + probeOffsets.size();
        for (const auto &entry : entries) {
          result[4 + entry.light / 32] |= 1u << (entry.light % 32);
          result[value++] = entry.light;
          result[value++] = std::bit_cast<uint32_t>(entry.visibility);
        }
        return;
      }
      result[0] = 0x56504c52u;
      result[1] = 3;
      result[2] = probes;
      result[3] = lights;
      std::copy(plan.slots.begin(), plan.slots.end(), result.begin() + 4);
      result[4 + lights] = plan.rowWords;
      const uint32_t rows = 5 + lights, offsets = rows + probes * plan.rowWords * 2;
      const uint32_t values = offsets + probes + 1;
      std::copy(probeOffsets.begin(), probeOffsets.end(), result.begin() + offsets);
      for (uint32_t probe = 0; probe < probes; ++probe) {
        for (uint32_t i = probeOffsets[probe]; i < probeOffsets[probe + 1]; ++i) {
          const auto &entry = entries[i];
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
    }
  } // namespace
  uint64_t ProbeVisibility::gpuBytes() const {
    const auto scalar = encoding(uint32_t(m_offsets.size() - 1), m_lights, m_entries,
        bytes() - m_direct.size() * sizeof(Direct), false).bytes;
    const uint64_t fields = nonzeroFieldCount(m_direct);
    const uint64_t directBytes = m_direct.size() * sizeof(uint32_t) + fields * sizeof(PackedUnitDirect);
    return m_direct.empty() ? scalar : (32 + scalar + directBytes + 15) & ~uint64_t(15);
  }
  std::vector<uint32_t> ProbeVisibility::words() const {
    const uint32_t probes = uint32_t(m_offsets.size() - 1);
    validate(probes, m_lights, m_offsets, m_entries, maximumBytes);
    const auto plan = encoding(probes, m_lights, m_entries, bytes() - m_direct.size() * sizeof(Direct), true);
    const size_t maskSize = size_t(plan.bytes / sizeof(uint32_t));
    if (m_direct.empty()) {
      std::vector<uint32_t> result(maskSize);
      writeMasks(result, plan, m_lights, m_offsets, m_entries);
      return result;
    }

    // Version seven wraps the same scalar payload with a directional directory.
    if (m_direct.size() >= UINT32_MAX) throw std::length_error("Too many unit-direct fields");
    const size_t fieldCount = nonzeroFieldCount(m_direct);
    constexpr uint32_t recordWords = PackedUnitDirect{}.size();
    const std::array header{0x56504c52u, 7u, probes, uint32_t(maskSize),
        uint32_t(m_direct.size()), uint32_t(fieldCount), recordWords, 0u};
    const size_t indexOffset = header.size() + maskSize;
    const size_t recordOffset = indexOffset + m_direct.size();
    const size_t words = recordOffset + fieldCount * recordWords;
    std::vector<uint32_t> result((words + 3) & ~size_t(3));
    std::copy(header.begin(), header.end(), result.begin());
    writeMasks(std::span(result).subspan(header.size(), maskSize), plan, m_lights, m_offsets, m_entries);
    auto record = result.begin() + recordOffset;
    uint32_t nextIndex = 1; // Zero is the shared black record.
    for (size_t i = 0; i < m_direct.size(); ++i) {
      const auto &field = m_direct[i];
      if (zeroUnitDirect(field)) continue;
      const auto packed = packUnitDirect(field);
      record = std::copy(packed.begin(), packed.end(), record);
      result[indexOffset + i] = nextIndex++;
    }
    return result;
  }
} // namespace remix::forward::bake
