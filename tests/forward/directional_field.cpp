#include "forward/mixed/directional_field.h"
#include "forward/mixed/directional_fields.h"
#include "forward/mixed/data.h"
#include "forward/half.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace field = remix::forward::mixed::directional;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  template <typename Function> void rejects(Function function) {
    bool rejected = false;
    try { function(); } catch (const std::exception &) { rejected = true; }
    require(rejected, "Malformed directional data must be rejected");
  }
  field::Encoded roundTrip(std::span<const field::Field> values, uint32_t width, uint32_t height) {
    auto encoded = field::encode(values, width, height);
    require(field::validate(encoded.words, width, height, encoded.encoding) == encoded.words.size(),
            "Directional chunks have an exact validated extent");
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x)
        require(field::texel(encoded.words, width, encoded.encoding, x, y) == values[size_t(y) * width + x],
                "Signed directional fields round-trip every half bit without spatial coarsening");
    return encoded;
  }
}
#include "directional_capture_cases.h"
int main() {
  directionalCaptureTests();
  {
    remix::forward::lightmap::Layout layout;
    layout.pageSize = 64; layout.pages = 1;
    require(remix::forward::mixed::directionalStorageBudget(layout) == (128ull << 20),
            "Small maps keep the bounded directional floor");
    layout.pageSize = 1024; layout.pages = 4;
    require(remix::forward::mixed::directionalStorageBudget(layout) == (288ull << 20),
            "Four atlas pages can reuse exactly the nine replaced RGBA16F layers");
    layout.pageSize = 4096;
    require(remix::forward::mixed::directionalStorageBudget(layout) == (512ull << 20),
            "Large atlas layouts cannot exceed the directional hard cap");
    layout.pages = 257;
    rejects([&] { (void)remix::forward::mixed::directionalStorageBudget(layout); });
    rejects([&] { remix::forward::mixed::DirectionalFields fields(1, 1,
        remix::forward::mixed::DirectionalFields::maximumBytes + 1); });
  }
  std::vector<field::Field> values(16 * 16);
  const auto black = roundTrip(values, 16, 16);
  require(black.encoding == field::tiled && black.words == std::vector<uint32_t>(4),
          "Entirely black blocks require only their descriptors");
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x) {
      auto &value = values[y * 16 + x];
      if (y < 8) {
        value[0] = remix::forward::half::encode(.5f);
        value[2] = remix::forward::half::encode(x < 8 ? -.2f : -float(x + y) * .013f);
      } else if (x >= 8) value[8] = 0x8000; // Preserve negative zero bits too.
    }
  const auto mixed = roundTrip(values, 16, 16);
  require(mixed.encoding == field::tiled && mixed.words[0] >> 30 == 1 && mixed.words[1] >> 30 == 2 &&
              mixed.words[2] == 0 && mixed.words[3] >> 30 == 1 && mixed.words.size() == 302,
          "Constant, raw and exactly black blocks share one lossless chunk");
  require(field::encode(values, 16, 16, {}, mixed.words.size()).words == mixed.words,
          "An exact encoding budget preserves every field and descriptor");
  rejects([&] { field::encode(values, 16, 16, {}, mixed.words.size() - 1); });
  rejects([&] { field::encode(values, 16, 16, {}, 0); });
  auto followed = mixed.words;
  followed.insert(followed.end(), 10, 0xffffffffu);
  require(field::validate(followed, 16, 16, mixed.encoding) == mixed.words.size(),
          "Validation stops at the current chunk before unrelated trailing data");
  uint32_t random = 17;
  for (const auto dimensions : {std::array<uint32_t, 2>{1, 1}, {1, 3}, {8, 8}, {9, 17}, {31, 23}, {1025, 1}}) {
    values.resize(size_t(dimensions[0]) * dimensions[1]);
    for (auto &value : values)
      for (auto &coefficient : value) {
        random = random * 1664525u + 1013904223u;
        coefficient = uint16_t(random & 0x83ffu); // Finite signed subnormals vary rapidly.
      }
    const auto encoded = roundTrip(values, dimensions[0], dimensions[1]);
    require(encoded.encoding == field::raw && encoded.words.size() == (values.size() * 9 + 1) / 2,
            "Raw storage bounds noisy, tiny, ragged and multi-batch directional images");
    if (values.size() & 1u) {
      auto broken = encoded.words;
      broken.back() |= 1u << 16;
      rejects([&] { field::validate(broken, dimensions[0], dimensions[1], field::raw); });
    }
  }
  values.assign(9 * 17, {});
  for (uint32_t y = 0; y < 17; ++y)
    values[y * 9 + 8][3] = remix::forward::half::encode(-.5f);
  roundTrip(values, 9, 17);
  auto malformed = mixed.words;
  malformed[0] += 1;
  rejects([&] { field::validate(malformed, 16, 16, field::tiled); });
  malformed = mixed.words;
  malformed[0] |= 3u << 30;
  rejects([&] { field::validate(malformed, 16, 16, field::tiled); });
  malformed = mixed.words;
  malformed[2] = 1;
  rejects([&] { field::validate(malformed, 16, 16, field::tiled); });
  malformed = mixed.words;
  malformed[4] = 0x7c00;
  rejects([&] { field::validate(malformed, 16, 16, field::tiled); });
  rejects([&] { field::validate(std::span(mixed.words).first(4), 16, 16, field::tiled); });
  rejects([&] { field::validate(mixed.words, 16, 16, 2); });
  rejects([&] { field::encode({}, 0, 1); });
  rejects([&] { field::encode(values, 4097, 1); });
  values[0][0] = 0x7e00;
  rejects([&] { field::encode(values, 9, 17); });
  values[0][0] = 0;
  std::stop_source cancelled;
  cancelled.request_stop();
  rejects([&] { field::encode(values, 9, 17, cancelled.get_token()); });
  {
    using remix::forward::mixed::DirectionalFields;
    const DirectionalFields empty(2, 0);
    const auto emptyWords = empty.words();
    DirectionalFields::validate(emptyWords, 2, 0, 4096);
    require(emptyWords.size() == DirectionalFields::headerWords + 3 &&
                !DirectionalFields::sample(emptyWords, 0, 0, 0, 0),
            "An empty sparse directory represents an atlas with no retained local attribution");
    DirectionalFields table(2, 16, 65536);
    const remix::forward::lightmap::Allocation rectangle{1, 30, 40, 16, 16};
    values.assign(16 * 16, {});
    require(table.add(1, 9, rectangle, values), "Black fields retain explicit chart/light ownership");
    for (uint32_t light : {15u, 7u, 6u, 5u, 4u, 3u, 2u, 0u}) {
      for (auto &value : values) value[2] = remix::forward::half::encode(-float(light + 1));
      require(table.add(0, light, rectangle, values), "Directional ownership has no four-light cap");
    }
    const auto words = table.words();
    DirectionalFields::validate(words, 2, 16, 65536);
    require(table.size() == 9 && words.size() * 4 == table.bytes(),
            "Scalable directory accounts for every owner and exact payload size");
    for (uint32_t light : {15u, 7u, 6u, 5u, 4u, 3u, 2u, 0u}) {
      field::Field expected{};
      expected[2] = remix::forward::half::encode(-float(light + 1));
      require(DirectionalFields::sample(words, 0, light, 15, 15) == expected,
              "Unordered submissions retain sorted sparse chart/light identity and signed fields");
    }
    require(DirectionalFields::sample(words, 1, 9, 0, 0) == field::Field{} &&
                !DirectionalFields::sample(words, 0, 9, 0, 0) &&
                !DirectionalFields::sample(words, 1, 9, 16, 0) &&
                !DirectionalFields::sample(words, 2, 0, 0, 0),
            "Known black, missing light, empty coverage and invalid coordinates remain distinct");
    rejects([&] { table.add(0, 0, rectangle, values); });
    rejects([&] { table.add(1, 1, rectangle, values, cancelled.get_token()); });
    require(table.words() == words, "Duplicate and cancelled additions do not mutate an existing table");
    rejects([&] { table.words(cancelled.get_token()); });
    DirectionalFields bounded(2, 16, 2 * 4 + 4 + DirectionalFields::headerWords * 4 +
                                           DirectionalFields::entryWords * 4 + 96);
    require(bounded.add(0, 0, rectangle, values), "A complete first field fits a bounded table");
    const auto before = bounded.words();
    require(!bounded.add(0, 1, rectangle, values) && bounded.words() == before,
            "Budget exhaustion keeps previously encoded ownership and data intact");
    auto broken = words;
    broken[broken[6] + DirectionalFields::entryWords] = broken[broken[6]];
    rejects([&] { DirectionalFields::validate(broken, 2, 16, 65536); });
    broken = words;
    broken[broken[6] + 6] = words[7] - 1;
    rejects([&] { DirectionalFields::validate(broken, 2, 16, 65536); });
    broken = words;
    broken[broken[5] + 1] = words[4] + 1;
    rejects([&] { DirectionalFields::validate(broken, 2, 16, 65536); });
    rejects([&] { DirectionalFields::validate(words, 2, 16, words.size() * 4 - 1); });
    rejects([&] { DirectionalFields invalid(2, 16, 0); });
  }
  {
    using namespace remix::forward;
    lightmap::Layout layout;
    layout.pageSize = 8;
    layout.pages = 2;
    layout.charts = {{0, 2, 3, 3, 3}, {1, 1, 1, 2, 2}};
    mixed::Plan plan;
    auto source = std::make_shared<remix::scene::BakeScene>();
    source->lights.resize(10);
    plan.lights.source = source;
    plan.directCharts = {{5, mixed::none, mixed::none, 2}, {9, mixed::none, mixed::none, mixed::none}};
    std::vector<uint16_t> atlas(layout.pages * layout.pageSize * layout.pageSize * lightmap::atlasLayers * 4);
    for (uint32_t i = 0; i < atlas.size(); ++i) atlas[i] = uint16_t(i & 0x83ffu);
    const auto original = atlas;
    const auto converted = mixed::retainedDirectionals(plan, layout, atlas);
    require(converted && converted->size() == 3, "Completed legacy owners migrate into one indexed table");
    const auto words = converted->words();
    mixed::DirectionalFields::validate(words, 2, 10, mixed::DirectionalFields::maximumBytes);
    for (uint32_t chart = 0; chart < 2; ++chart) {
      const auto &r = layout.charts[chart];
      for (uint32_t channel = 0; channel < 4; ++channel) {
        const auto light = plan.directCharts[chart][channel];
        if (light == mixed::none) continue;
        for (uint32_t y = 0; y < r.height; ++y)
          for (uint32_t x = 0; x < r.width; ++x) {
            field::Field expected;
            for (uint32_t c = 0; c < 9; ++c)
              expected[c] = atlas[((r.page * lightmap::atlasLayers + lightmap::directLayer + c) * 64 +
                  (r.y + y) * 8 + r.x + x) * 4 + channel];
            require(mixed::DirectionalFields::sample(words, chart, light, x, y) == expected,
                    "Migration preserves every source half across pages, chart offsets and sparse channels");
          }
      }
    }
    require(atlas == original && !mixed::retainedDirectionals(plan, layout, atlas, 64),
            "Migration leaves the cache atlas untouched and declines incomplete budget replacements");
  }
  std::puts("Directional surface blocks preserve signed half fields, padding, bounds and cancellation");
}
