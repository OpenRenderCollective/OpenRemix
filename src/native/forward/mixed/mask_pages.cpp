#include "forward/mixed/mask_pages.h"
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace remix::forward::mixed {
  std::vector<uint32_t> MaskPages::words(uint32_t charts) const {
    if (rectangles.empty()) return {};
    if (size > 65535 || layers > 65536)
      throw std::length_error("Visibility page lookup exceeds packed coordinate range");
    std::vector<uint32_t> result(size_t(charts) * 2 + rectangles.size() * 4);
    for (size_t i = 0; i < rectangles.size(); ++i) {
      const auto &r = rectangles[i];
      if (r.chart >= charts)
        throw std::invalid_argument("Invalid visibility page chart");
      const uint32_t record = uint32_t(size_t(charts) * 2 + i * 4);
      auto &count = result[r.chart * 2 + 1];
      if (!count++) result[r.chart * 2] = record;
      result[record] = r.light;
      result[record + 1] = r.x | (r.y << 16);
      result[record + 2] = r.width | 0x40000000u;
      result[record + 3] = r.height | (r.layer << 16);
    }
    return result;
  }
  namespace {
    struct Segment {
      uint32_t x, y, width;
    };
    struct Position {
      uint32_t layer, x, y;
    };
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Visibility page packing superseded");
    }
    // A skyline describes the first free row of every horizontal interval.
    // Rectangles can bridge several intervals; choose the lowest resulting
    // top edge, then layer/x for deterministic reuse of existing pages.
    std::optional<Position> find(const std::vector<std::vector<Segment>> &pages,
                                 uint32_t size, uint32_t width, uint32_t height) {
      std::optional<Position> best;
      for (uint32_t layer = 0; layer < pages.size(); ++layer) {
        const auto &skyline = pages[layer];
        for (size_t first = 0; first < skyline.size(); ++first) {
          const uint32_t x = skyline[first].x;
          if (width > size - x)
            continue;
          uint32_t y = 0, remaining = width;
          for (size_t i = first; remaining; ++i) {
            y = std::max(y, skyline[i].y);
            remaining -= std::min(remaining, skyline[i].width);
          }
          if (height > size - y)
            continue;
          if (!best || std::tie(y, layer, x) < std::tie(best->y, best->layer, best->x))
            best = Position{layer, x, y};
        }
      }
      return best;
    }
    void occupy(std::vector<Segment> &skyline, Position p, uint32_t width, uint32_t height) {
      std::vector<Segment> next;
      for (const auto &s : skyline) {
        if (s.x < p.x)
          next.push_back({s.x, s.y, std::min(s.width, p.x - s.x)});
        if (s.x <= p.x && p.x < s.x + s.width)
          next.push_back({p.x, p.y + height, width});
        if (s.x + s.width > p.x + width) {
          const uint32_t start = std::max(s.x, p.x + width);
          next.push_back({start, s.y, s.x + s.width - start});
        }
      }
      skyline.clear();
      for (const auto &s : next) {
        if (!skyline.empty() && skyline.back().y == s.y)
          skyline.back().width += s.width;
        else
          skyline.push_back(s);
      }
    }
    struct PagePlan {
      MaskPages pages;
      std::vector<size_t> order;
    };
    std::optional<PagePlan> planPages(const Masks &masks, uint32_t pageSize,
                                      uint32_t maximumLayers, uint64_t maximumBytes,
                                      std::stop_token cancelled) {
      check(cancelled);
      if (!pageSize || !maximumLayers)
        throw std::invalid_argument("Invalid visibility page limits");
      PagePlan plan;
      auto &result = plan.pages;
      result.size = pageSize;
      const auto entries = masks.entries();
      if (entries.empty())
        return plan;
      const uint64_t pageBytes = uint64_t(pageSize) * pageSize;
      const uint64_t tableBytes = uint64_t(entries.size()) * sizeof(MaskPages::Rectangle);
      if (tableBytes > maximumBytes || pageBytes > maximumBytes - tableBytes)
        return {};
      const uint64_t limit = std::min(uint64_t(maximumLayers), (maximumBytes - tableBytes) / pageBytes);
      auto &order = plan.order;
      order.resize(entries.size());
      std::iota(order.begin(), order.end(), 0);
      for (const auto &entry : entries) {
        check(cancelled);
        if (entry.width > pageSize || entry.height > pageSize)
          return {};
        result.occupiedTexels += uint64_t(entry.width) * entry.height;
      }
      if (result.occupiedTexels > limit * pageBytes)
        return {};
      std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const auto &x = entries[a], &y = entries[b];
        if (x.height != y.height) return x.height > y.height;
        if (x.width != y.width) return x.width > y.width;
        return std::tie(x.chart, x.light) < std::tie(y.chart, y.light);
      });
      std::vector<std::vector<Segment>> pages;
      result.rectangles.reserve(entries.size());
      for (const auto index : order) {
        check(cancelled);
        const auto &entry = entries[index];
        auto p = find(pages, pageSize, entry.width, entry.height);
        if (!p) {
          if (pages.size() == limit)
            return {};
          p = Position{uint32_t(pages.size()), 0, 0};
          pages.push_back({{0, 0, pageSize}});
        }
        occupy(pages[p->layer], *p, entry.width, entry.height);
        result.rectangles.push_back({entry.chart, entry.light, p->layer, p->x, p->y,
                                     entry.width, entry.height});
      }
      result.layers = uint32_t(pages.size());
      return plan;
    }
    MaskPages decodePages(const Masks &masks, PagePlan plan, std::stop_token cancelled) {
      auto &result = plan.pages;
      const auto entries = masks.entries();
      const auto &order = plan.order;
      const uint32_t pageSize = result.size;
      const uint64_t pageBytes = uint64_t(pageSize) * pageSize;
      check(cancelled);
      result.texels.resize(size_t(pageBytes * result.layers), 255);
      for (size_t i = 0; i < order.size(); ++i) {
        const auto &entry = entries[order[i]];
        const auto &r = result.rectangles[i];
        const auto bytes = masks.bytes().subspan(entry.offset);
        for (uint32_t y = 0; y < r.height; ++y) {
          check(cancelled);
          const size_t offset = (size_t(r.layer) * pageSize + r.y + y) * pageSize + r.x;
          for (uint32_t x = 0; x < r.width; ++x)
            result.texels[offset + x] = mask::texel(bytes, r.width, entry.encoding, x, y);
        }
      }
      std::sort(result.rectangles.begin(), result.rectangles.end(), [](const auto &a, const auto &b) {
        return std::tie(a.chart, a.light) < std::tie(b.chart, b.light);
      });
      return std::move(result);
    }
  } // namespace
  std::optional<MaskPages> packMaskPages(const Masks &masks, uint32_t pageSize,
                                       uint32_t maximumLayers, uint64_t maximumBytes,
                                       std::stop_token cancelled) {
    auto plan = planPages(masks, pageSize, maximumLayers, maximumBytes, cancelled);
    if (!plan) return {};
    return decodePages(masks, std::move(*plan), cancelled);
  }
  std::optional<MaskPages> selectMaskPages(const Masks &masks, uint32_t charts,
                                         uint32_t maximumDimension, uint32_t maximumLayers,
                                         std::stop_token cancelled) {
    check(cancelled);
    if (masks.entries().empty() || !maximumLayers)
      return {};
    uint64_t bestBytes = masks.words(charts).size() * 4ull; // R32_UINT lookup/payload
    std::optional<PagePlan> best;
    for (uint32_t size = 64; size <= maximumDimension && size <= 32768; size *= 2) {
      if (uint64_t(size) * size >= Masks::maximumBytes)
        break;
      auto candidate = planPages(masks, size, std::min(maximumLayers, 65536u),
                                     Masks::maximumBytes, cancelled);
      if (!candidate || !candidate->pages.layers)
        continue;
      // Lookup size is fixed by chart/rectangle counts. Compare layouts without
      // allocating images or decoding masks that will be discarded.
      const auto &pages = candidate->pages;
      const uint64_t bytes = uint64_t(size) * size * pages.layers +
          (uint64_t(charts) * 2 + pages.rectangles.size() * 4ull) * sizeof(uint32_t);
      if (bytes < bestBytes && bytes <= Masks::maximumBytes) {
        bestBytes = bytes;
        best = std::move(candidate);
      }
    }
    if (!best) return {};
    return decodePages(masks, std::move(*best), cancelled);
  }
} // namespace remix::forward::mixed
