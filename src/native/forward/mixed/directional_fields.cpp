#include "forward/mixed/directional_fields.h"
#include "forward/mixed/data.h"
#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace remix::forward::mixed {
  namespace {
    void require(bool valid) {
      if (!valid) throw std::invalid_argument("Invalid directional surface table");
    }
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Directional surface table superseded");
    }
    template <typename T> void reserve(std::vector<T> &values, size_t size, size_t maximum) {
      if (size > values.capacity())
        values.reserve(std::max(size, std::min(maximum, std::max(size_t(8), values.capacity() * 2))));
    }
  }
  DirectionalFields::DirectionalFields(uint32_t charts, uint32_t lights, uint64_t budget)
      : m_charts(charts), m_lights(lights), m_budget(budget) {
    require(charts < UINT32_MAX && lights < (1u << 24) && budget <= uint64_t(UINT32_MAX) * 4);
    if (bytes() > budget) throw std::length_error("Directional surface directory exceeds budget");
  }
  uint64_t DirectionalFields::bytes() const {
    return (uint64_t(headerWords) + m_charts + 1 + m_entries.size() * entryWords + m_data.size()) * 4;
  }
  bool DirectionalFields::add(uint32_t chart, uint32_t light, const lightmap::Allocation &rectangle,
      std::span<const directional::Field> fields, std::stop_token cancelled) {
    check(cancelled);
    require(chart < m_charts && light < m_lights && rectangle.width > 0 && rectangle.width <= 4096 &&
        rectangle.height > 0 && rectangle.height <= 4096 && rectangle.x <= 4096 - rectangle.width &&
        rectangle.y <= 4096 - rectangle.height && !m_identities.contains({chart, light}));
    if (bytes() + entryWords * 4 > m_budget) return false;
    directional::Encoded encoded;
    try {
      encoded = directional::encode(fields, rectangle.width, rectangle.height, cancelled,
          (m_budget - bytes() - entryWords * 4) / 4);
    } catch (const std::length_error &) {
      return false;
    }
    if (bytes() + entryWords * 4 + encoded.words.size() * 4 > m_budget) return false;
    reserve(m_entries, m_entries.size() + 1, size_t(m_budget / (entryWords * 4)));
    reserve(m_data, m_data.size() + encoded.words.size(), size_t(m_budget / 4));
    check(cancelled);
    // Allocate before committing. Budget failure, cancellation and allocation
    // exceptions cannot leave an identity pointing to an incomplete payload.
    m_identities.emplace(chart, light);
    m_entries.push_back({chart, light, rectangle.x, rectangle.y, rectangle.width, rectangle.height,
        encoded.encoding, uint32_t(m_data.size()), uint32_t(encoded.words.size())});
    m_data.insert(m_data.end(), encoded.words.begin(), encoded.words.end());
    return true;
  }
  std::vector<uint32_t> DirectionalFields::words(std::stop_token cancelled) const {
    check(cancelled);
    const uint32_t offsets = headerWords, entries = offsets + m_charts + 1;
    const uint32_t data = entries + uint32_t(m_entries.size()) * entryWords;
    std::vector<uint32_t> result(size_t(bytes() / 4), 0);
    const uint32_t header[] = {magic, version, m_charts, m_lights, uint32_t(m_entries.size()),
        offsets, entries, data, uint32_t(result.size()), 0, 0, 0};
    std::copy(std::begin(header), std::end(header), result.begin());
    std::vector<uint32_t> order(m_entries.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const auto &x = m_entries[a], &y = m_entries[b];
      return x.chart < y.chart || (x.chart == y.chart && x.light < y.light);
    });
    uint32_t next = 0;
    for (uint32_t chart = 0; chart < m_charts; ++chart) {
      check(cancelled);
      result[offsets + chart] = next;
      while (next < order.size() && m_entries[order[next]].chart == chart) {
        const auto &entry = m_entries[order[next]];
        const uint32_t record[] = {entry.light, entry.x, entry.y, entry.width, entry.height,
            entry.encoding, data + entry.first, entry.count};
        std::copy(std::begin(record), std::end(record), result.begin() + entries + size_t(next) * entryWords);
        ++next;
      }
    }
    result[offsets + m_charts] = next;
    for (size_t first = 0; first < m_data.size(); first += 65536) {
      check(cancelled);
      const size_t count = std::min(size_t(65536), m_data.size() - first);
      std::copy_n(m_data.begin() + first, count, result.begin() + data + first);
    }
    return result;
  }
  void DirectionalFields::validate(std::span<const uint32_t> words, uint32_t charts, uint32_t lights,
                                    uint64_t budget) {
    require(words.size() >= headerWords && words.size_bytes() <= budget && words.size() <= UINT32_MAX &&
        charts < UINT32_MAX && lights < (1u << 24));
    require(words[0] == magic && words[1] == version && words[2] == charts && words[3] == lights &&
        words[5] == headerWords && words[6] == uint64_t(headerWords) + charts + 1 &&
        words[7] == uint64_t(words[6]) + uint64_t(words[4]) * entryWords && words[8] == words.size() &&
        words[9] == 0 && words[10] == 0 && words[11] == 0 && words[7] <= words.size());
    const uint32_t offsets = words[5], entries = words[6];
    require(words[offsets] == 0 && words[offsets + charts] == words[4]);
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    ranges.reserve(words[4]);
    for (uint32_t chart = 0; chart < charts; ++chart) {
      const auto first = words[offsets + chart], end = words[offsets + chart + 1];
      require(first <= end && end <= words[4]);
      uint32_t previous = 0;
      for (uint32_t i = first; i < end; ++i) {
        const auto record = words.subspan(entries + size_t(i) * entryWords, entryWords);
        require(record[0] < lights && (i == first || record[0] > previous) &&
            record[3] > 0 && record[3] <= 4096 && record[4] > 0 && record[4] <= 4096 &&
            record[1] <= 4096 - record[3] && record[2] <= 4096 - record[4] &&
            record[6] >= words[7] && record[6] <= words.size() && record[7] <= words.size() - record[6]);
        previous = record[0];
        require(directional::validate(words.subspan(record[6], record[7]), record[3], record[4], record[5]) == record[7]);
        ranges.emplace_back(record[6], record[7]);
      }
    }
    std::sort(ranges.begin(), ranges.end());
    uint64_t cursor = words[7];
    for (const auto &[first, count] : ranges) {
      require(first == cursor);
      cursor += count;
    }
    require(cursor == words.size());
  }
  std::optional<directional::Field> DirectionalFields::sample(std::span<const uint32_t> words,
      uint32_t chart, uint32_t light, uint32_t x, uint32_t y) {
    if (chart >= words[2] || light >= words[3]) return {};
    uint32_t first = words[words[5] + chart], end = words[words[5] + chart + 1];
    const auto last = end;
    while (first < end) {
      const auto middle = first + (end - first) / 2;
      if (words[words[6] + size_t(middle) * entryWords] < light) first = middle + 1;
      else end = middle;
    }
    if (first == last) return {};
    const auto record = words.subspan(words[6] + size_t(first) * entryWords, entryWords);
    if (record[0] != light || x >= record[3] || y >= record[4]) return {};
    return directional::texel(words.subspan(record[6], record[7]), record[3], record[5], x, y);
  }
  std::optional<DirectionalFields> retainedDirectionals(const Plan &plan, const lightmap::Layout &layout,
      std::span<const uint16_t> pixels, uint64_t budget, std::stop_token cancelled) {
    check(cancelled);
    require(plan.lights.source && plan.lights.source->lights.size() < (1u << 24) &&
        plan.directCharts.size() == layout.charts.size() &&
        layout.pageSize > 0 && layout.pageSize <= 4096 && layout.pages <= 256 &&
        layout.charts.size() < UINT32_MAX &&
        pixels.size() == uint64_t(layout.pageSize) * layout.pageSize * layout.pages * lightmap::atlasLayers * 4);
    if ((uint64_t(DirectionalFields::headerWords) + layout.charts.size() + 1) * 4 > budget) return {};
    DirectionalFields result(uint32_t(layout.charts.size()), uint32_t(plan.lights.source->lights.size()), budget);
    std::vector<directional::Field> fields;
    const size_t layerPixels = size_t(layout.pageSize) * layout.pageSize;
    for (uint32_t chart = 0; chart < layout.charts.size(); ++chart) {
      check(cancelled);
      const auto &rectangle = layout.charts[chart];
      require(rectangle.page < layout.pages && rectangle.width > 0 && rectangle.width <= layout.pageSize &&
          rectangle.height > 0 && rectangle.height <= layout.pageSize &&
          rectangle.x <= layout.pageSize - rectangle.width && rectangle.y <= layout.pageSize - rectangle.height);
      for (uint32_t channel = 0; channel < channels; ++channel) {
        const auto light = plan.directCharts[chart][channel];
        if (light == none) continue;
        fields.resize(size_t(rectangle.width) * rectangle.height);
        for (uint32_t y = 0; y < rectangle.height; ++y) {
          check(cancelled);
          for (uint32_t x = 0; x < rectangle.width; ++x) {
            auto &field = fields[size_t(y) * rectangle.width + x];
            const size_t texel = size_t(rectangle.y + y) * layout.pageSize + rectangle.x + x;
            for (uint32_t c = 0; c < field.size(); ++c)
              field[c] = pixels[((size_t(rectangle.page) * lightmap::atlasLayers + lightmap::directLayer + c) *
                  layerPixels + texel) * 4 + channel];
          }
        }
        if (!result.add(chart, light, rectangle, fields, cancelled)) return {};
      }
    }
    return result;
  }
}
