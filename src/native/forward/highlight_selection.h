#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace remix::forward {
  // Selection of baked-light highlights is independent of shadow-map slots.
  // Callers supply only fixed lights with attributed static visibility. Keys
  // identify captured emitters (not adapter light IDs, which may be shared).
  struct HighlightCandidate {
    uint64_t key;
    float priority;
  };
  struct HighlightWeight {
    uint64_t key;
    float value;
  };
  class HighlightSelection {
  public:
    static constexpr uint32_t maximum = 32;

    void reset() { *this = {}; }

    std::vector<HighlightWeight> update(std::span<const HighlightCandidate> input, uint32_t budget,
                                        double seconds, float fadeSeconds) {
      std::vector<HighlightCandidate> candidates;
      for (const auto &candidate : input)
        if (std::isfinite(candidate.priority) && candidate.priority > 0)
          candidates.push_back(candidate);
      std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        return a.key < b.key;
      });
      for (size_t i = 1; i < candidates.size(); ++i)
        if (candidates[i - 1].key == candidates[i].key)
          throw std::invalid_argument("Highlight candidates must identify distinct captured emitters");
      std::sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        return a.priority != b.priority ? a.priority > b.priority : a.key < b.key;
      });
      budget = std::min(budget, maximum);
      const size_t wantedCount = std::min(size_t(budget), candidates.size());
      const auto present = [&](uint64_t key) {
        return std::any_of(candidates.begin(), candidates.end(),
                           [&](const auto &candidate) { return candidate.key == key; });
      };
      const auto wanted = [&](uint64_t key) {
        return std::any_of(candidates.begin(), candidates.begin() + wantedCount,
                           [&](const auto &candidate) { return candidate.key == key; });
      };
      double elapsed = 0;
      if (std::isfinite(seconds)) {
        if (clockStarted) elapsed = std::max(0., seconds - previousSeconds);
        previousSeconds = clockStarted ? std::max(previousSeconds, seconds) : seconds;
        clockStarted = true;
      }
      const bool fading = std::isfinite(fadeSeconds) && fadeSeconds > 0;
      const float step = fading ? float(std::min(1., elapsed / fadeSeconds)) : 1.f;
      for (uint32_t i = 0; i < maximum; ++i) {
        auto &slot = slots[i];
        // A reduced budget is an explicit policy change, with a hard cost cap.
        if (i >= budget || (slot.used && !present(slot.key))) slot = {};
        if (!slot.used) continue;
        const bool keep = wanted(slot.key);
        slot.weight = std::clamp(slot.weight + (keep ? step : -step), 0.f, 1.f);
        if (!keep && slot.weight == 0) slot = {};
      }
      for (size_t i = 0; i < wantedCount; ++i) {
        const auto key = candidates[i].key;
        if (std::any_of(slots.begin(), slots.end(),
                        [&](const auto &slot) { return slot.used && slot.key == key; })) continue;
        const auto free = std::find_if(slots.begin(), slots.begin() + budget,
                                       [](const auto &slot) { return !slot.used; });
        // Outgoing highlights retain their bounded slot until their fade ends.
        if (free == slots.begin() + budget) break;
        *free = {key, fading ? 0.f : 1.f, true};
      }
      std::vector<HighlightWeight> result;
      for (const auto &slot : slots)
        if (slot.used) result.push_back({slot.key, slot.weight});
      return result;
    }

  private:
    struct Slot {
      uint64_t key = 0;
      float weight = 0;
      bool used = false;
    };
    std::array<Slot, maximum> slots{};
    double previousSeconds = 0;
    bool clockStarted = false;
  };
} // namespace remix::forward
