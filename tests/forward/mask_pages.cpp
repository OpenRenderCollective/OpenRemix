#include "forward/mixed/mask_pages.h"
#include "forward/mixed/mask_lookup.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace remix::forward::mixed;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  void verify(const Masks &masks, const MaskPages &pages) {
    require(pages.texels.size() == uint64_t(pages.size) * pages.size * pages.layers,
            "Page allocation accounts for all layer padding");
    std::vector<bool> occupied(pages.texels.size());
    require(pages.rectangles.size() == masks.entries().size(), "Every mask keeps its identity");
    uint64_t count = 0;
    for (const auto &r : pages.rectangles) {
      const auto found = std::find_if(masks.entries().begin(), masks.entries().end(), [&](const auto &e) {
        return e.chart == r.chart && e.light == r.light;
      });
      require(found != masks.entries().end(), "Packed chart/light identity remains valid");
      require(r.layer < pages.layers && r.x + r.width <= pages.size && r.y + r.height <= pages.size,
              "Every rectangle fits its assigned layer");
      const auto bytes = masks.bytes().subspan(found->offset);
      for (uint32_t y = 0; y < r.height; ++y)
        for (uint32_t x = 0; x < r.width; ++x) {
          const size_t address = (size_t(r.layer) * pages.size + r.y + y) * pages.size + r.x + x;
          require(!occupied[address], "Different chart/light rectangles never alias");
          occupied[address] = true;
          require(pages.texels[address] == mask::texel(bytes, r.width, found->encoding, x, y),
                  "R8 pages preserve every decoded visibility texel exactly");
          ++count;
        }
    }
    require(count == pages.occupiedTexels, "Occupancy excludes page padding");
  }
}
int main() {
  {
    std::vector<uint32_t> records(2 + 8 * 4);
    records[0] = 2;
    records[1] = 8;
    for (uint32_t i = 0; i < 8; ++i) {
      records[2 + i * 4] = i * 3;
      records[3 + i * 4] = 1234 + i;
    }
    const auto original = records;
    indexMaskRecords(records, 1, 0);
    require(records == original, "Insufficient directory budget preserves compact lookup exactly");
    indexMaskRecords(records, 1);
    require(records[1] == (0x80000000u | 22u), "Directory bounds include the highest light");
    for (uint32_t light = 0; light < 22; ++light) {
      const uint32_t record = records[records[0] + light];
      require(record == (light % 3 ? 0u : 2u + light / 3 * 4),
              "Directory preserves sparse light identity and missing entries");
      if (record) require(records[record + 1] == 1234 + light / 3,
                          "Directory retains original payload addresses");
    }
    const auto indexed = records;
    indexMaskRecords(records, 1);
    require(records == indexed, "Directory publication is idempotent");
    auto sparse = original;
    sparse[2 + 7 * 4] = 0x7fffffffu;
    const auto oversized = sparse;
    indexMaskRecords(sparse, 1);
    require(sparse == oversized, "Unrepresentable light spans retain compact lookup");
    auto compactChart = original;
    compactChart[1] = 7;
    const auto compact = compactChart;
    indexMaskRecords(compactChart, 1);
    require(compactChart == compact, "Small charts avoid directory allocation");
    std::vector<uint32_t> mixedCharts(4 + 9 * 4);
    mixedCharts[0] = 4;
    mixedCharts[1] = 8;
    mixedCharts[2] = 36;
    mixedCharts[3] = 1;
    for (uint32_t i = 0; i < 8; ++i) mixedCharts[4 + i * 4] = i;
    mixedCharts[36] = 99;
    indexMaskRecords(mixedCharts, 2);
    require(mixedCharts[1] == (0x80000000u | 8u) && mixedCharts[2] == 36 &&
                mixedCharts[3] == 1 && mixedCharts[36] == 99,
            "Indexed and compact charts coexist without relocating records");
  }
  remix::forward::lightmap::Layout layout;
  layout.charts = {{0, 0, 0, 8, 8}, {0, 0, 0, 5, 3}, {0, 0, 0, 1, 13}, {0, 0, 0, 7, 9}};
  std::vector<Masks::Entry> entries;
  std::vector<uint8_t> bytes;
  for (uint32_t light = 0; light < 13; ++light) {
    const uint32_t chart = light < 7 ? 0 : light % uint32_t(layout.charts.size());
    const auto &a = layout.charts[chart];
    std::vector<uint8_t> texels(size_t(a.width) * a.height);
    for (size_t i = 0; i < texels.size(); ++i)
      texels[i] = light % 3 == 0 ? uint8_t(light * 17) : light % 3 == 1 ?
                  (i % 3 ? 255 : 0) : uint8_t((i * 29 + light * 37) & 255);
    const auto encoded = mask::encodeCompact(texels, a.width, a.height);
    entries.push_back({chart, light, uint32_t(bytes.size()), a.width, a.height, encoded.encoding});
    bytes.insert(bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
  }
  const auto masks = Masks::restore(entries, bytes, layout, 13);
  const auto pages = packMaskPages(masks, 16, 8, 4096);
  require(bool(pages) && pages->layers > 1 && pages->layers < entries.size(),
          "Thirteen lights reuse a smaller number of layers");
  verify(masks, *pages);
  const auto lookup = pages->words(uint32_t(layout.charts.size()));
  for (const auto &r : pages->rectangles) {
    const uint32_t first = lookup[r.chart * 2], count = lookup[r.chart * 2 + 1];
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
      const size_t record = first + i * 4;
      if (lookup[record] != r.light) continue;
      require(lookup[record + 1] == (r.x | (r.y << 16)) &&
              lookup[record + 2] == (r.width | 0x40000000u) &&
              lookup[record + 3] == (r.height | (r.layer << 16)),
              "Runtime page lookup preserves rectangle coordinates and layer");
      found = true;
    }
    require(found, "Each light has a searchable runtime page record");
  }
  uint32_t overlaps = 0;
  for (const auto &r : pages->rectangles) overlaps += r.chart == 0;
  require(overlaps > 4, "More than four masks on one receiver retain independent identities");
  const auto repeat = packMaskPages(masks, 16, 8, 4096);
  require(repeat->texels == pages->texels && repeat->layers == pages->layers,
          "Packing and decoding are deterministic");
  const uint64_t exactBytes = pages->texels.size() + pages->rectangles.size() * sizeof(MaskPages::Rectangle);
  require(bool(packMaskPages(masks, 16, pages->layers, exactBytes)), "Exact allocation budget fits");
  require(!packMaskPages(masks, 16, pages->layers, exactBytes - 1), "Padding and table bytes count against budget");
  require(!packMaskPages(masks, 16, pages->layers - 1, 4096), "Array layer limit is enforced");
  require(!packMaskPages(masks, 8, 64, 4096), "Oversized rectangles preserve compressed fallback");
  const auto empty = packMaskPages({}, 16, 8, 0);
  require(empty && empty->layers == 0 && empty->texels.empty(), "Absent masks allocate no layers");
  std::stop_source cancelled;
  cancelled.request_stop();
  bool stopped = false;
  try { (void)packMaskPages(masks, 16, 8, 4096, cancelled.get_token()); }
  catch (const std::runtime_error &) { stopped = true; }
  require(stopped, "Cancellation never returns partial published pages");
  // High-entropy masks benefit from R8 pages; constant/paletted maps can stay
  // compressed. Selection must compare allocated storage, not occupied area.
  std::vector<Masks::Entry> rawEntries;
  std::vector<uint8_t> rawBytes;
  for (uint32_t light = 0; light < 100; ++light) {
    std::vector<uint8_t> texels(64);
    for (uint32_t i = 0; i < 64; ++i) texels[i] = uint8_t(i * 37 + light * 11);
    const auto encoded = mask::encode(texels, 8, 8);
    rawEntries.push_back({0, light, uint32_t(rawBytes.size()), 8, 8, encoded.encoding});
    rawBytes.insert(rawBytes.end(), encoded.bytes.begin(), encoded.bytes.end());
  }
  const auto raw = Masks::restore(rawEntries, rawBytes, layout, 100);
  const auto selected = selectMaskPages(raw, uint32_t(layout.charts.size()), 1024, 256);
  require(bool(selected), "R8 selection recognizes savings for high-entropy masks");
  verify(raw, *selected);
  require(selected->texels.size() + selected->words(uint32_t(layout.charts.size())).size() * 8 <
            raw.words(uint32_t(layout.charts.size())).size() * 8,
          "Selected page representation reduces total payload and lookup storage");
  require(!selectMaskPages(raw, uint32_t(layout.charts.size()), 32, 1),
          "Devices without a fitting page retain compressed masks");
  std::vector<Masks::Entry> constantEntries;
  std::vector<uint8_t> constantBytes;
  const auto constant = mask::encode(std::vector<uint8_t>(64, 73), 8, 8);
  for (uint32_t light = 0; light < 100; ++light) {
    constantEntries.push_back({0, light, uint32_t(constantBytes.size()), 8, 8, constant.encoding});
    constantBytes.insert(constantBytes.end(), constant.bytes.begin(), constant.bytes.end());
  }
  const auto compact = Masks::restore(constantEntries, constantBytes, layout, 100);
  require(!selectMaskPages(compact, uint32_t(layout.charts.size()), 1024, 256),
          "Already compact masks are not expanded into more expensive R8 pages");
  std::printf("Visibility pages: %zu masks, %u layers, %llu/%zu occupied texels; seven coincident lights preserved\n",
              entries.size(), pages->layers, static_cast<unsigned long long>(pages->occupiedTexels),
              pages->texels.size());
}
