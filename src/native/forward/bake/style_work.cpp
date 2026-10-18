#include "forward/bake/style_work.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace remix::forward::bake {
  StyleWork::StyleWork(const scene::BakeScene &source, const lightmap::Layout &layout,
                       const Grid &base, uint64_t maximumBytes)
      : m_styles(styleIds(source)), m_probes(base.probes.size()),
        m_builder({layout.pageSize, layout.pages, uint32_t(base.probes.size())}, maximumBytes) {
    for (uint32_t i = 0; i < base.probes.size(); ++i)
      if (base.probes[i].statistics[3] > .5f) m_probeTasks.push_back(i);
    m_probeWritten.resize(m_probeTasks.size());
  }
  uint32_t StyleWork::style() const { return m_styles.at(m_current); }
  bool StyleWork::received() const {
    return probeReceived == m_probeTasks.size() && surfaceReceived == m_surfaces.size();
  }
  void StyleWork::takeSurfaces(std::vector<Result> &completedBase, size_t count) {
    m_surfaces.swap(completedBase);
    m_surfaces.resize(count);
    m_surfaceWritten.assign(count, 0);
  }
  void StyleWork::consume(uint32_t group, bool surface, uint32_t first, std::span<const Result> values) {
    if (complete() || group != style() ||
        uint64_t(first) + values.size() > (surface ? m_surfaces.size() : m_probeTasks.size()))
      throw std::invalid_argument("Invalid attributed lighting readback ownership");
    auto &written = surface ? m_surfaceWritten : m_probeWritten;
    for (uint32_t i = 0; i < values.size(); ++i)
      if (written[first + i]) throw std::invalid_argument("Duplicate attributed lighting readback");
    for (const auto &value : values)
      for (const auto &coefficient : value.irradiance)
        for (uint32_t c = 0; c < 3; ++c)
          if (!std::isfinite(coefficient[c]))
            throw std::invalid_argument("Non-finite attributed irradiance");
    for (uint32_t i = 0; i < values.size(); ++i) {
      (surface ? m_surfaces[first + i] : m_probes[m_probeTasks[first + i]]) = values[i];
      written[first + i] = 1;
    }
    (surface ? surfaceReceived : probeReceived) += uint32_t(values.size());
  }
  void StyleWork::encode(const lightmap::Layout &layout, bool filter, std::stop_token cancelled,
                         std::function<void(uint32_t, uint32_t)> progress) {
    if (complete() || !received()) throw std::logic_error("Attributed lighting readbacks are incomplete");
    if (filter && !m_surfaces.empty()) {
      const auto result = lightmap::denoise(layout, m_surfaces, cancelled, std::move(progress));
      std::fprintf(stderr, "Forward styled indirect OIDN: style %u, %u charts, %.3f s\n",
                   style(), result.charts, result.seconds);
    }
    m_builder.add(style(), layout, m_surfaces, m_probes, cancelled);
    ++m_current;
    std::fill(m_surfaceWritten.begin(), m_surfaceWritten.end(), 0);
    std::fill(m_probeWritten.begin(), m_probeWritten.end(), 0);
    probeNext = probeReceived = surfaceNext = surfaceReceived = 0;
  }
  std::vector<uint32_t> StyleWork::finish(std::stop_token cancelled) {
    if (!complete()) throw std::logic_error("Attributed lighting groups are incomplete");
    std::vector<Result>().swap(m_surfaces);
    std::vector<Result>().swap(m_probes);
    return m_builder.serialize(cancelled);
  }
} // namespace remix::forward::bake
