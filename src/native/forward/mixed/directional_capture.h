#pragma once
#include "forward/mixed/directional_fields.h"
#include "forward/mixed/directional_links.h"

namespace remix::forward::mixed {
  // A bounded window of complete chart/light fields shares transport batches.
  // Layout must remain immutable and alive until collection finishes. This is
  // independent of GPU fences; sample indices are local to the current window.
  // The caller must drain all in-flight receipts before advancing a window.
  class DirectionalCapture {
  public:
    using Link = DirectionalLink;
    struct Field { Link owner; uint32_t first, count; };
    static constexpr uint32_t maximumWindowFields = 256;
    DirectionalCapture(const lightmap::Layout &layout, uint32_t lights, std::span<const Link> links,
                       uint64_t stagingBudget, uint64_t storageBudget = DirectionalFields::defaultBytes,
                       std::stop_token cancelled = {});
    bool done() const { return m_next == m_links.size(); }
    uint32_t begin(std::stop_token cancelled = {});
    // pointTarget is soft for the first nonempty field; stagingBudget remains
    // hard. This permits a large field without splitting its rectangle.
    uint32_t beginWindow(uint32_t pointTarget, uint32_t fieldLimit = maximumWindowFields,
                         std::stop_token cancelled = {});
    std::span<const Field> window() const { return m_window; } // valid until finish()
    std::vector<bake::VisibilityTask> batch(uint32_t first, uint32_t count) const;
    std::vector<bake::VisibilityTask> batch(std::span<const uint32_t> indices) const;
    void observe(uint32_t first, std::span<const bake::VisibilityResult> values,
                 std::stop_token cancelled = {});
    void observe(std::span<const uint32_t> indices, std::span<const bake::VisibilityResult> values,
                 std::stop_token cancelled = {});
    // False declines the entire replacement on encoded-storage exhaustion.
    // Partial fields and incomplete manifests cannot be exported by words().
    bool finish(std::stop_token cancelled = {});
    std::vector<uint32_t> words(std::stop_token cancelled = {}) const;
    // Resident index/sample staging. beginWindow() also budgets receipt-index
    // validation and one temporary rectangle. Codec scratch/output are separate.
    uint64_t stagingBytes() const;
    DirectionalFields::StorageUsage storageUsage() const { return m_fields.storageUsage(); }

  private:
    const lightmap::Layout &m_layout;
    std::vector<Link> m_links;
    std::vector<uint32_t> m_offsets, m_points;
    std::vector<Field> m_window;
    DirectionalFields m_fields;
    uint64_t m_budget;
    size_t m_next = 0;
    bool m_active = false, m_failed = false;
    uint32_t m_received = 0;
    std::vector<directional::Field> m_samples;
    std::vector<uint8_t> m_seen;
    bake::VisibilityTask task(uint32_t sample) const;
  };
}
