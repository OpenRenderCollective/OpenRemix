#pragma once
#include "forward/mixed/directional_fields.h"
#include "forward/mixed/directional_links.h"

namespace remix::forward::mixed {
  // Field-major readback collection: only one chart/light's samples are staged.
  // Layout must remain immutable and alive until collection finishes. This is
  // independent of GPU fences; first/count are local to the current field.
  // The caller must finish all in-flight receipts before advancing a field.
  class DirectionalCapture {
  public:
    using Link = DirectionalLink;
    DirectionalCapture(const lightmap::Layout &layout, uint32_t lights, std::span<const Link> links,
                       uint64_t stagingBudget, uint64_t storageBudget = DirectionalFields::defaultBytes,
                       std::stop_token cancelled = {});
    bool done() const { return m_next == m_links.size(); }
    uint32_t begin(std::stop_token cancelled = {});
    std::vector<bake::VisibilityTask> batch(uint32_t first, uint32_t count) const;
    void observe(uint32_t first, std::span<const bake::VisibilityResult> values,
                 std::stop_token cancelled = {});
    // False declines the entire replacement on encoded-storage exhaustion.
    // Partial fields and incomplete manifests cannot be exported by words().
    bool finish(std::stop_token cancelled = {});
    std::vector<uint32_t> words(std::stop_token cancelled = {}) const;
    // Resident index/sample staging. begin() also budgets the temporary output
    // rectangle. Encoding scratch and the bounded published table are separate.
    uint64_t stagingBytes() const;

  private:
    const lightmap::Layout &m_layout;
    std::vector<Link> m_links;
    std::vector<uint32_t> m_offsets, m_points;
    DirectionalFields m_fields;
    uint64_t m_budget;
    size_t m_next = 0;
    bool m_active = false, m_failed = false;
    uint32_t m_received = 0;
    std::vector<directional::Field> m_samples;
    std::vector<uint8_t> m_seen;
  };
}
