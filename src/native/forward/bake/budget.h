#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace remix::forward::bake {
  enum class Work : uint32_t { Irradiance, Surface, Visibility, Sun, Site, Count };
  // GPU timing guides throughput, never sample count or bounce quality. Manual
  // batches remain available, and every choice obeys the hard ray-work ceiling.
  class Budget {
  public:
    struct Statistics {
      uint64_t points = 0, batches = 0;
      double milliseconds = 0, longest = 0;
      uint32_t largest = 0;
    };
    const Statistics &statistics(Work work) const { return m_states[size_t(work)].statistics; }
    // Vulkan guarantees at least 65535 workgroups on X. One parallel group
    // shades a point; the time estimate, not a tiny fixed launch, sets occupancy.
    static constexpr uint32_t maximumPoints = 65535, maximumWork = 16u << 20;
    static constexpr uint32_t manualWork = 1u << 20;
    static constexpr uint32_t minimumAutomaticPoints = 64; // avoid learning from an underfilled GPU
    uint32_t count(Work work, uint32_t initial, float milliseconds, uint32_t samples, uint32_t bounces,
                   uint32_t remaining) {
      auto &state = m_states[size_t(work)];
      if (!state.points)
        state.points = initial;
      if (milliseconds > 0)
        state.points = std::max(minimumAutomaticPoints, state.points);
      const uint32_t desired = milliseconds > 0 ? state.points : initial;
      // Timed batches can use more of a fast RT device. Keep the legacy manual
      // ceiling on hosts without timestamps, where there is no feedback.
      const uint32_t limit = (milliseconds > 0 ? maximumWork : manualWork) /
                             std::max(1u, samples * bounces);
      return std::min({remaining, std::max(1u, desired), maximumPoints, limit});
    }
    void observe(Work work, uint32_t points, double elapsed, float target) {
      auto &state = m_states[size_t(work)];
      if (!points || !std::isfinite(elapsed) || elapsed <= 0 || !std::isfinite(target) || target <= 0)
        return;
      auto &statistics = state.statistics;
      statistics.points += points;
      ++statistics.batches;
      statistics.milliseconds += elapsed;
      statistics.longest = std::max(statistics.longest, elapsed);
      statistics.largest = std::max(statistics.largest, points);
      // Small tails overemphasize fixed dispatch overhead. A large overrun must
      // still reduce work immediately, even on such a tail.
      if (points < state.points / 4 && elapsed <= target)
        return;
      const double wanted = double(points) * target / elapsed;
      const uint32_t next =
          uint32_t(std::clamp(wanted, double(minimumAutomaticPoints), double(maximumPoints)));
      if (elapsed > target)
        state.points = std::min(state.points, next);
      else
        state.points = std::min(next, std::max(1u, state.points) * 2);
    }

  private:
    struct State {
      uint32_t points = 0;
      Statistics statistics;
    };
    std::array<State, size_t(Work::Count)> m_states{};
  };
} // namespace remix::forward::bake
