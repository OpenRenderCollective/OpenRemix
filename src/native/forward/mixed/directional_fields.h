#pragma once
#include "forward/mixed/directional_field.h"
#include "forward/lightmap/layout.h"
#include <optional>
#include <set>

namespace remix::forward::mixed {
  struct Plan;
  // Sparse chart/light ownership is independent of the legacy four atlas
  // channels. A known black field has an entry; an absent entry is unknown.
  class DirectionalFields {
  public:
    static constexpr uint32_t magic = 0x48534452u, version = 1, headerWords = 12, entryWords = 8;
    static constexpr uint64_t defaultBytes = 128ull << 20, maximumBytes = 512ull << 20;
    DirectionalFields(uint32_t charts, uint32_t lights, uint64_t budget = defaultBytes);
    bool add(uint32_t chart, uint32_t light, const lightmap::Allocation &rectangle,
             std::span<const directional::Field> fields, std::stop_token cancelled = {});
    uint64_t bytes() const;
    size_t size() const { return m_entries.size(); }
    std::vector<uint32_t> words(std::stop_token cancelled = {}) const;
    static void validate(std::span<const uint32_t> words, uint32_t charts, uint32_t lights, uint64_t budget);
    // CPU reference; call only on a validated table. Coordinates are local to
    // the chart rectangle. Absent/out-of-range is distinct from black.
    static std::optional<directional::Field> sample(std::span<const uint32_t> words,
        uint32_t chart, uint32_t light, uint32_t x, uint32_t y);

  private:
    struct Entry { uint32_t chart, light, x, y, width, height, encoding, first, count; };
    uint32_t m_charts, m_lights;
    uint64_t m_budget;
    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_data;
    std::set<std::pair<uint32_t, uint32_t>> m_identities;
  };
  // Migrate currently retained cache/atlas fields into scalable storage without
  // modifying source pixels or advertising additional, untraced light owners.
  // Budget failure declines the entire replacement, never a subset of owners.
  std::optional<DirectionalFields> retainedDirectionals(const Plan &plan, const lightmap::Layout &layout,
      std::span<const uint16_t> pixels, uint64_t budget = DirectionalFields::defaultBytes,
      std::stop_token cancelled = {});
  // Complete fields replace nine RGBA16F atlas layers. Reuse that storage
  // allowance on large maps, with a small-map floor and a bounded hard cap.
  uint64_t directionalStorageBudget(const lightmap::Layout &layout);
}
