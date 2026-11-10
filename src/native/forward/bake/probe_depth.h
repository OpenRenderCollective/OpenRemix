#pragma once
#include "forward/lighting.h"
#include <optional>
#include <span>
#include <vector>

namespace remix::forward::bake {
  // Directional first-hit moments. Four geometry rays per octahedral texel;
  // normalized half pairs keep one probe in the existing 256-byte result slot.
  struct ProbeDepth {
    static constexpr uint32_t side = 8, texels = side * side, samples = 4;
    using Record = std::array<uint32_t, texels>;
    float maximumDistance = 0;
    std::vector<Record> records;
    bool empty() const { return records.empty(); }
    uint64_t bytes() const { return empty() ? 0 : 16 + uint64_t(records.size()) * sizeof(Record); }
    void validate(uint32_t probeCount) const;
    void write(std::span<std::byte> output) const;
    std::optional<std::array<float, 2>> moments(uint32_t probe, Vec3 direction) const;
    std::optional<float> visibility(uint32_t probe, Vec3 delta, float minimumVariance) const;
    static Vec3 direction(uint32_t texel, uint32_t sample);
  };
}
