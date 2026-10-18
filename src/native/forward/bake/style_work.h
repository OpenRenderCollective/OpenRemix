#pragma once
#include "forward/bake/grid.h"
#include "forward/bake/sparse_sh.h"
#include "forward/lightmap/denoise.h"

namespace remix::forward::bake {
  // One reusable readback workspace for all attributed groups. Base probe
  // geometry remains authoritative; a style owns RGB coefficients only.
  class StyleWork {
  public:
    StyleWork(const scene::BakeScene &source, const lightmap::Layout &layout,
              const Grid &base, uint64_t maximumBytes);
    uint32_t style() const;
    uint32_t ordinal() const { return uint32_t(m_current + 1); }
    uint32_t count() const { return uint32_t(m_styles.size()); }
    bool complete() const { return m_current == m_styles.size(); }
    bool received() const;
    void takeSurfaces(std::vector<Result> &completedBase, size_t count);
    void consume(uint32_t style, bool surface, uint32_t first, std::span<const Result> values);
    void encode(const lightmap::Layout &layout, bool denoise, std::stop_token cancelled,
                std::function<void(uint32_t, uint32_t)> progress = {});
    std::vector<uint32_t> finish(std::stop_token cancelled);
    const std::vector<uint32_t> &probeTasks() const { return m_probeTasks; }
    uint32_t surfaceCount() const { return uint32_t(m_surfaces.size()); }
    uint32_t probeNext = 0, probeReceived = 0, surfaceNext = 0, surfaceReceived = 0;

  private:
    std::vector<uint32_t> m_styles, m_probeTasks;
    size_t m_current = 0;
    std::vector<Result> m_surfaces, m_probes;
    std::vector<uint8_t> m_surfaceWritten, m_probeWritten;
    sparse::Builder m_builder;
  };
} // namespace remix::forward::bake
