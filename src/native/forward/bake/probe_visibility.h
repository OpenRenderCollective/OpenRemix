#pragma once
#include "forward/bake/data.h"
#include <optional>
#include <span>

namespace remix::forward::bake {
// Sparse probe-major local-light visibility. Light numbers are captured-scene
// indices, never lightstyle numbers or transient runtime light-list offsets.
// Unknown is distinct from clear: incomplete data must keep shadow fallback.
class ProbeVisibility {
public:
  struct Entry {
    uint32_t light;
    float visibility = -1;
  };
  static_assert(sizeof(Entry) == 8);
  // Input may be unordered. Duplicate pairs are rejected. The complete table
  // must fit its budget; silently dropping pairs would advertise false
  // coverage.
  ProbeVisibility(uint32_t probes, uint32_t lights,
                  std::span<const VisibilityTask> pairs, uint64_t byteBudget);
  std::span<const uint32_t> offsets() const { return m_offsets; }
  std::span<const Entry> entries() const { return m_entries; }
  uint64_t bytes() const;
  std::optional<float> sample(uint32_t probe, uint32_t light) const;
  // Validate the entire readback before publishing any value. Each pair is
  // written once; a failed/incomplete batch cannot turn unknown into clear.
  void observe(std::span<const VisibilityTask> pairs,
               std::span<const float> visibility);

private:
  std::vector<uint32_t> m_offsets;
  std::vector<Entry> m_entries;
  uint32_t m_lights = 0;
  uint32_t find(uint32_t probe, uint32_t light) const;
};
} // namespace remix::forward::bake
