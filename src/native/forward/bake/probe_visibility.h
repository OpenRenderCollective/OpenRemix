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
    static constexpr uint64_t maximumBytes = 64ull << 20;
    static constexpr uint64_t maximumDirectoryBytes = 8ull << 20;
    struct Entry {
      uint32_t light;
      float visibility = -1;
    };
    static_assert(sizeof(Entry) == 8);
    // Unit-radiance convolved SH for aggregate moving-shadow corrections.
    // Sparse attribution has no four-light channel limit; it shares mask IDs.
    using Direct = std::array<float, 9>;
    static_assert(sizeof(Direct) == 36);
    // Input may be unordered. Duplicate pairs are rejected. The complete table
    // must fit its budget; silently dropping pairs would advertise false
    // coverage.
    ProbeVisibility(uint32_t probes, uint32_t lights, std::span<const VisibilityTask> pairs,
                    uint64_t byteBudget, bool directional = false);
    std::span<const Direct> direct() const { return m_direct; }
    bool directional() const { return !m_direct.empty(); }
    std::span<const uint32_t> offsets() const { return m_offsets; }
    std::span<const Entry> entries() const { return m_entries; }
    uint64_t bytes() const;
    uint64_t gpuBytes() const;
    // Versioned shader words: compact selected-light directory, ranked probe
    // membership and exact float masks. Oversized directories use the bounded
    // availability/offset/sorted-entry representation. Both are padded to vec4.
    // Optional version-five wrapper adds signed directional coefficients.
    std::vector<uint32_t> words() const;
    static void validate(uint32_t probes, uint32_t lights, std::span<const uint32_t> offsets,
                         std::span<const Entry> entries, uint64_t byteBudget);
    static ProbeVisibility restore(uint32_t probes, uint32_t lights, std::span<const uint32_t> offsets,
                                   std::span<const Entry> entries, uint64_t byteBudget,
                                   std::span<const Direct> direct = {});
    std::optional<float> sample(uint32_t probe, uint32_t light) const;
    std::optional<Direct> sampleDirect(uint32_t probe, uint32_t light) const;
    void observe(std::span<const VisibilityTask> pairs, std::span<const VisibilityResult> results);
    // Validate the entire readback before publishing any value. Each pair is
    // written once; a failed/incomplete batch cannot turn unknown into clear.
    void observe(std::span<const VisibilityTask> pairs, std::span<const float> visibility);

  private:
    ProbeVisibility() = default;
    std::vector<uint32_t> m_offsets;
    std::vector<Entry> m_entries;
    std::vector<Direct> m_direct; // optional; same probe/light ordering as entries
    uint32_t m_lights = 0;
    uint32_t find(uint32_t probe, uint32_t light) const;
    std::vector<uint32_t> maskWords() const;
    void observeMasks(std::span<const VisibilityTask> pairs, std::span<const float> visibility);
  };
} // namespace remix::forward::bake
