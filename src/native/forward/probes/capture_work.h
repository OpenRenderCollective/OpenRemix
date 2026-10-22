#pragma once
#include "forward/probes/data.h"
#include <utility>

namespace remix::forward::probes {
  // A lighting query set independent of the selected projection boxes. Each
  // bit-identical origin/resolution is traced once. Captures retain separate
  // radiance arrays and projection records for publication and persistence.
  class CaptureWork {
  public:
    CaptureWork() = default;
    explicit CaptureWork(std::span<const Capture> captures, std::stop_token cancelled = {});
    // Cache loads need provenance, but no transport work. Hash one unique
    // cubemap at a time instead of allocating the entire scene's ray list.
    static uint64_t inputHash(std::span<const Capture> captures, std::stop_token cancelled = {});
    std::span<const bake::Point> rays() const { return m_rays; }
    // Readback ray -> owning capture and its row-major face texel.
    std::pair<uint32_t, uint32_t> destination(uint32_t ray) const;
    // After all readbacks complete, populate same-origin alias arrays. The
    // capture order, origins and resolutions must still match the input.
    void complete(std::span<Capture> captures, std::stop_token cancelled = {}) const;

  private:
    CaptureWork(std::span<const Capture> captures, std::stop_token cancelled, bool generate);
    uint32_t m_texels = 0, m_resolution = 0;
    std::vector<uint32_t> m_owners, m_sources;
    std::vector<Vec3> m_positions;
    std::vector<bake::Point> m_rays;
  };
} // namespace remix::forward::probes
