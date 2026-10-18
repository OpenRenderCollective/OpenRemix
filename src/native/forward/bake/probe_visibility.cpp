#include "forward/bake/probe_visibility.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace remix::forward::bake {
  std::vector<uint32_t> ProbeVisibility::words() const {
    validate(uint32_t(m_offsets.size() - 1), m_lights, m_offsets, m_entries, maximumBytes);
    std::vector<uint32_t> result{0x56504c52u, 1u, uint32_t(m_offsets.size() - 1), uint32_t(m_entries.size())};
    result.insert(result.end(), m_offsets.begin(), m_offsets.end());
    for (const auto &entry : m_entries) {
      result.push_back(entry.light);
      result.push_back(std::bit_cast<uint32_t>(entry.visibility));
    }
    result.resize((result.size() + 3) & ~size_t(3));
    return result;
  }
  void ProbeVisibility::validate(uint32_t probes, uint32_t lights, std::span<const uint32_t> offsets,
                                 std::span<const Entry> entries, uint64_t byteBudget) {
    if (probes == UINT32_MAX || offsets.size() != uint64_t(probes) + 1 || offsets.size_bytes() > byteBudget ||
        entries.size_bytes() > byteBudget - offsets.size_bytes() || entries.size() >= UINT32_MAX ||
        offsets.front() != 0 || offsets.back() != entries.size())
      throw std::invalid_argument("Invalid completed probe visibility storage");
    for (uint32_t probe = 0; probe < probes; ++probe) {
      if (offsets[probe] > offsets[probe + 1] || offsets[probe + 1] > entries.size())
        throw std::invalid_argument("Invalid probe visibility offsets");
      for (uint32_t i = offsets[probe]; i < offsets[probe + 1]; ++i) {
        const auto &entry = entries[i];
        if (entry.light >= lights || !std::isfinite(entry.visibility) || entry.visibility < 0 ||
            entry.visibility > 1 || (i > offsets[probe] && entries[i - 1].light >= entry.light))
          throw std::invalid_argument("Invalid completed probe visibility entry");
      }
    }
  }
  ProbeVisibility ProbeVisibility::restore(uint32_t probes, uint32_t lights,
                                           std::span<const uint32_t> offsets, std::span<const Entry> entries,
                                           uint64_t byteBudget) {
    validate(probes, lights, offsets, entries, byteBudget);
    ProbeVisibility result;
    result.m_lights = lights;
    result.m_offsets.assign(offsets.begin(), offsets.end());
    result.m_entries.assign(entries.begin(), entries.end());
    return result;
  }
  ProbeVisibility::ProbeVisibility(uint32_t probes, uint32_t lights, std::span<const VisibilityTask> pairs,
                                   uint64_t byteBudget)
      : m_lights(lights) {
    const uint64_t required = (uint64_t(probes) + 1) * sizeof(uint32_t) + pairs.size() * sizeof(Entry);
    if (probes == UINT32_MAX || pairs.size() >= UINT32_MAX || required > byteBudget)
      throw std::length_error("Probe visibility exceeds its storage budget");
    for (const auto &pair : pairs)
      if (pair.point >= probes || pair.light >= lights)
        throw std::invalid_argument("Invalid probe visibility pair");
    std::vector<VisibilityTask> ordered(pairs.begin(), pairs.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
      return a.point < b.point || (a.point == b.point && a.light < b.light);
    });
    for (size_t i = 1; i < ordered.size(); ++i)
      if (ordered[i].point == ordered[i - 1].point && ordered[i].light == ordered[i - 1].light)
        throw std::invalid_argument("Duplicate probe visibility pair");
    m_offsets.resize(size_t(probes) + 1);
    m_entries.reserve(ordered.size());
    size_t next = 0;
    for (uint32_t probe = 0; probe < probes; ++probe) {
      m_offsets[probe] = uint32_t(next);
      while (next < ordered.size() && ordered[next].point == probe)
        m_entries.push_back({ordered[next++].light, -1});
    }
    m_offsets.back() = uint32_t(next);
  }
  uint64_t ProbeVisibility::bytes() const {
    return m_offsets.size() * sizeof(uint32_t) + m_entries.size() * sizeof(Entry);
  }
  uint32_t ProbeVisibility::find(uint32_t probe, uint32_t light) const {
    if (uint64_t(probe) + 1 >= m_offsets.size() || light >= m_lights)
      return UINT32_MAX;
    const auto begin = m_entries.begin() + m_offsets[probe], end = m_entries.begin() + m_offsets[probe + 1];
    const auto found = std::lower_bound(
        begin, end, light, [](const Entry &entry, uint32_t value) { return entry.light < value; });
    return found != end && found->light == light ? uint32_t(found - m_entries.begin()) : UINT32_MAX;
  }
  std::optional<float> ProbeVisibility::sample(uint32_t probe, uint32_t light) const {
    const uint32_t index = find(probe, light);
    return index != UINT32_MAX && m_entries[index].visibility >= 0
               ? std::optional(m_entries[index].visibility)
               : std::nullopt;
  }
  void ProbeVisibility::observe(std::span<const VisibilityTask> pairs, std::span<const float> visibility) {
    if (pairs.size() != visibility.size())
      throw std::invalid_argument("Probe visibility readback count mismatch");
    std::vector<uint32_t> indices;
    indices.reserve(pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i) {
      const uint32_t index = find(pairs[i].point, pairs[i].light);
      if (index == UINT32_MAX || m_entries[index].visibility >= 0 || !std::isfinite(visibility[i]) ||
          visibility[i] < 0 || visibility[i] > 1)
        throw std::invalid_argument("Invalid probe visibility readback");
      indices.push_back(index);
    }
    auto ordered = indices;
    std::sort(ordered.begin(), ordered.end());
    if (std::adjacent_find(ordered.begin(), ordered.end()) != ordered.end())
      throw std::invalid_argument("Duplicate probe visibility readback");
    for (size_t i = 0; i < indices.size(); ++i)
      m_entries[indices[i]].visibility = visibility[i];
  }
} // namespace remix::forward::bake
