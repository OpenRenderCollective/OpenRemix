#include "forward/lightmap/denoise_chart.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace remix::forward::lightmap::denoising {
  namespace {
    constexpr size_t outside = size_t(-1);
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Lightmap denoising superseded");
    }
  } // namespace
  uint32_t batchFields(size_t fieldBytes) {
    return uint32_t(
        std::clamp(batchBudgetBytes / std::max(size_t(1), fieldBytes), size_t(1), size_t(coefficientCount)));
  }
  uint32_t alignedExtent(uint32_t extent, uint32_t alignment) {
    if (!extent || !alignment || alignment > 16)
      throw std::invalid_argument("Invalid denoising image alignment");
    const auto padded = ((uint64_t(extent) + alignment - 1) / alignment) * alignment;
    if (padded > std::numeric_limits<uint32_t>::max())
      throw std::invalid_argument("Denoising image extent overflow");
    return uint32_t(padded);
  }
  Chart::Chart(const Layout &layout, uint32_t chart, std::span<bake::Result> results,
               std::span<const uint32_t> points, std::stop_token cancelled, uint32_t alignment)
      : m_layout(layout), m_allocation(layout.charts.at(chart)), m_chart(chart),
        m_width(alignedExtent(m_allocation.width, alignment)),
        m_height(alignedExtent(m_allocation.height, alignment)),
        m_batch(batchFields(size_t(m_width) * m_height * 3 * sizeof(float))),
        m_origin((size_t(m_allocation.page) * layout.pageSize + m_allocation.y) * layout.pageSize +
                 m_allocation.x),
        m_plane(size_t(m_width) * m_height * 3), m_results(results), m_points(points), m_cancelled(cancelled) {
    check(m_cancelled);
    m_original.resize(m_plane);
    m_l0.resize(m_plane);
    m_fields.resize(m_plane * m_batch);
    for (uint32_t y = 0; y < m_allocation.height; ++y) {
      check(m_cancelled);
      for (uint32_t x = 0; x < m_allocation.width; ++x) {
        const auto donor = owner(x, y);
        for (uint32_t c = 0; c < 3; ++c) {
          const float value = m_results[donor].irradiance[0][c];
          if (!std::isfinite(value) || value < 0)
            throw std::invalid_argument("Invalid indirect L0");
          m_original[(size_t(y) * m_width + x) * 3 + c] = value;
        }
      }
    }
  }
  size_t Chart::owner(uint32_t x, uint32_t y) const {
    const auto donor = m_layout.owners.at(m_origin + size_t(y) * m_layout.pageSize + x);
    if (donor >= m_results.size() || m_layout.pointCharts.at(donor) != m_chart)
      throw std::invalid_argument("Lightmap denoising requires chart-local dilation donors");
    return donor;
  }
  size_t Chart::pixel(uint32_t point) const {
    const size_t address = m_layout.addresses.at(point);
    if (address < m_origin)
      return outside;
    const auto offset = address - m_origin, x = offset % m_layout.pageSize, y = offset / m_layout.pageSize;
    if (x >= m_allocation.width || y >= m_allocation.height)
      return outside;
    return (y * m_width + x) * 3;
  }
  uint64_t Chart::hostBytes() const {
    return (m_original.size() + m_l0.size() + m_fields.size()) * sizeof(float);
  }
  std::span<float> Chart::pack(uint32_t first, uint32_t count) {
    if (!count || count > m_batch || first >= coefficientCount || count > coefficientCount - first)
      throw std::invalid_argument("Invalid lightmap coefficient batch");
    auto output = std::span(m_fields).first(m_plane * count);
    for (uint32_t slot = 0; slot < count; ++slot) {
      check(m_cancelled);
      const uint32_t coefficient = first + slot;
      for (uint32_t y = 0; y < m_allocation.height; ++y) {
        check(m_cancelled);
        for (uint32_t x = 0; x < m_allocation.width; ++x) {
          const auto donor = owner(x, y);
          for (uint32_t c = 0; c < 3; ++c) {
            const size_t k = (size_t(y) * m_width + x) * 3 + c;
            float value = m_results[donor].irradiance[coefficient][c];
            if (!std::isfinite(value))
              throw std::invalid_argument("Invalid indirect SH band");
            // Convolved SH has |Ln/L0| <= 1.155 for L1 and .56 for L2.
            // Divide by two to fit the directional filter's [-1,1] domain.
            if (coefficient)
              value = m_original[k] > 1e-10f ? std::clamp(value / (2 * m_original[k]), -1.f, 1.f) : 0;
            output[size_t(slot) * m_plane + k] = value;
          }
        }
      }
      // Alignment is an implementation detail, not a black lighting boundary.
      // Continue only this chart's values into the temporary right/bottom pad.
      // Signed SH padding at -1 can otherwise create coloured reconstruction
      // fringes, even though the HDR image looks smoothly denoised.
      if (m_width != m_allocation.width || m_height != m_allocation.height)
        for (uint32_t y = 0; y < m_height; ++y) {
          check(m_cancelled);
          for (uint32_t x = 0; x < m_width; ++x)
            if (x >= m_allocation.width || y >= m_allocation.height) {
              const size_t source = size_t(slot)*m_plane +
                  (size_t(std::min(y, m_allocation.height-1))*m_width + std::min(x, m_allocation.width-1))*3;
              const size_t target = size_t(slot)*m_plane + (size_t(y)*m_width+x)*3;
              std::copy_n(output.begin()+source, 3, output.begin()+target);
            }
        }
    }
    return output;
  }
  void Chart::apply(uint32_t first, uint32_t count, std::span<const float> filtered) {
    if (!count || count > m_batch || first >= coefficientCount || count > coefficientCount - first ||
        filtered.size() != m_plane * count)
      throw std::invalid_argument("Invalid filtered lightmap batch");
    if (!first)
      std::copy_n(filtered.begin(), m_plane, m_l0.begin());
    for (const auto point : m_points) {
      check(m_cancelled);
      const auto k = pixel(point);
      if (k == outside)
        continue; // extended seam donors outside this rectangle remain raw
      for (uint32_t coefficient = std::max(1u, first); coefficient < first + count; ++coefficient)
        for (uint32_t c = 0; c < 3; ++c)
          m_results[point].irradiance[coefficient][c] =
              std::clamp(filtered[size_t(coefficient - first) * m_plane + k + c], -1.f, 1.f) * 2 *
              std::max(m_l0[k + c], 0.f);
    }
  }
  void Chart::finish() {
    for (const auto point : m_points) {
      check(m_cancelled);
      const auto k = pixel(point);
      if (k == outside)
        continue;
      for (uint32_t c = 0; c < 3; ++c)
        m_results[point].irradiance[0][c] = std::max(m_l0[k + c], 0.f);
    }
  }
} // namespace remix::forward::lightmap::denoising
