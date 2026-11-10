#include "forward/bake/probe_visibility.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace remix::forward::bake {
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
                                           uint64_t byteBudget, std::span<const Direct> direct) {
    if ((!direct.empty() && direct.size() != entries.size()) || direct.size_bytes() > byteBudget)
      throw std::invalid_argument("Invalid directional probe storage");
    for (const auto &value : direct)
      for (float coefficient : value)
        if (!std::isfinite(coefficient))
          throw std::invalid_argument("Non-finite directional probe coefficient");
    validate(probes, lights, offsets, entries, byteBudget - direct.size_bytes());
    ProbeVisibility result;
    result.m_lights = lights;
    result.m_offsets.assign(offsets.begin(), offsets.end());
    result.m_entries.assign(entries.begin(), entries.end());
    result.m_direct.assign(direct.begin(), direct.end());
    return result;
  }
  ProbeVisibility::ProbeVisibility(uint32_t probes, uint32_t lights, std::span<const VisibilityTask> pairs,
                                   uint64_t byteBudget, bool directional)
      : m_lights(lights) {
    const uint64_t stride = sizeof(Entry) + (directional ? sizeof(Direct) : 0);
    const uint64_t required = (uint64_t(probes) + 1) * sizeof(uint32_t) + pairs.size() * stride;
    if (probes == UINT32_MAX || pairs.size() >= UINT32_MAX || required > byteBudget)
      throw std::length_error("Probe visibility exceeds its storage budget");
    m_offsets.resize(size_t(probes) + 1);
    for (const auto &pair : pairs) {
      if (pair.point >= probes || pair.light >= lights)
        throw std::invalid_argument("Invalid probe visibility pair");
      ++m_offsets[pair.point];
    }
    // Prefix sums give each probe an exclusive end. Filling each range
    // backwards turns those ends into the final starts in place.
    std::partial_sum(m_offsets.begin(), m_offsets.end(), m_offsets.begin());
    m_entries.resize(pairs.size());
    for (const auto &pair : pairs)
      m_entries[--m_offsets[pair.point]] = {pair.light, -1};
    for (uint32_t probe = 0; probe < probes; ++probe) {
      const auto first = m_entries.begin() + m_offsets[probe];
      const auto last = m_entries.begin() + m_offsets[probe + 1];
      if (last - first < 2) continue;
      std::sort(first, last, [](const Entry &a, const Entry &b) { return a.light < b.light; });
      if (std::adjacent_find(first, last, [](const Entry &a, const Entry &b) { return a.light == b.light; }) != last)
        throw std::invalid_argument("Duplicate probe visibility pair");
    }
    if (directional) m_direct.resize(m_entries.size());
  }
  uint64_t ProbeVisibility::bytes() const {
    return m_offsets.size() * sizeof(uint32_t) + m_entries.size() * sizeof(Entry) +
        m_direct.size() * sizeof(Direct);
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
  std::optional<ProbeVisibility::Direct> ProbeVisibility::sampleDirect(uint32_t probe, uint32_t light) const {
    const uint32_t index = find(probe, light);
    if (m_direct.empty() || index == UINT32_MAX || m_entries[index].visibility < 0) return {};
    return m_direct[index];
  }
  void ProbeVisibility::observe(std::span<const VisibilityTask> pairs,
                                 std::span<const VisibilityResult> results) {
    if (m_direct.empty() || pairs.size() != results.size())
      throw std::invalid_argument("Directional probe readback count mismatch");
    const auto indices = readbackIndices(pairs, [&](size_t i) {
      const auto &result = results[i];
      for (float coefficient : result.irradiance)
        if (!std::isfinite(coefficient))
          throw std::invalid_argument("Non-finite directional probe readback");
      return result.visibility;
    });
    for (size_t i = 0; i < indices.size(); ++i) {
      m_entries[indices[i]].visibility = results[i].visibility;
      m_direct[indices[i]] = results[i].irradiance;
    }
  }
  void ProbeVisibility::observe(std::span<const VisibilityTask> pairs, std::span<const float> visibility) {
    if (directional())
      throw std::invalid_argument("Directional probes require complete directional readback");
    if (pairs.size() != visibility.size())
      throw std::invalid_argument("Probe visibility readback count mismatch");
    const auto indices = readbackIndices(pairs, [&](size_t i) { return visibility[i]; });
    for (size_t i = 0; i < indices.size(); ++i)
      m_entries[indices[i]].visibility = visibility[i];
  }
  template <class VisibilityAt>
  std::vector<uint32_t> ProbeVisibility::readbackIndices(std::span<const VisibilityTask> pairs,
                                                        VisibilityAt visibilityAt) const {
    std::vector<uint32_t> indices;
    indices.reserve(pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i) {
      const uint32_t index = find(pairs[i].point, pairs[i].light);
      const float visibility = visibilityAt(i);
      if (index == UINT32_MAX || m_entries[index].visibility >= 0 || !std::isfinite(visibility) ||
          visibility < 0 || visibility > 1)
        throw std::invalid_argument("Invalid probe visibility readback");
      indices.push_back(index);
    }
    std::vector<uint32_t> sorted;
    std::span<const uint32_t> ordered = indices;
    if (!std::is_sorted(indices.begin(), indices.end())) {
      sorted = indices;
      std::sort(sorted.begin(), sorted.end());
      ordered = sorted;
    }
    if (std::adjacent_find(ordered.begin(), ordered.end()) != ordered.end())
      throw std::invalid_argument("Duplicate probe visibility readback");
    return indices;
  }
} // namespace remix::forward::bake
