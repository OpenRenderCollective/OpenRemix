#include "forward/bake/sparse_sh.h"
#include "forward/half.h"
#include "forward/packed_rgb.h"
#include "forward/lightmap/data.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace remix;
using namespace remix::forward;
namespace b = remix::forward::bake;
namespace s = b::sparse;
namespace lm = remix::forward::lightmap;
namespace {
  void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
  template <class F> void refused(F &&function, const char *message) {
    bool failed = false;
    try { function(); } catch (const std::exception &) { failed = true; }
    require(failed, message);
  }
}
int main() {
  for (int exponent = -24; exponent <= 16; ++exponent)
    for (uint32_t sample = 0; sample < 1000; ++sample) {
      const float amplitude = std::ldexp(1.f, exponent);
      const std::array<float,3> original{amplitude * float(sample) / 999,
          -amplitude * float((sample * 37) % 1000) / 999, amplitude * .0031f};
      const auto packed = packSignedRgb(original);
      const auto decoded = unpackSignedRgb(packed);
      const float step = std::ldexp(1.f, int(packed >> 27) - 22);
      for (uint32_t c = 0; c < 3; ++c)
        require(std::abs(decoded[c] - std::clamp(original[c], -65000.f, 65000.f)) <= step * .5001f,
                "Signed HDR RGB stays within half a quantization step for each channel");
      require(packSignedRgb(decoded) == packed, "Packed SH has a canonical stable representation");
    }

  lm::Layout layout;
  layout.pageSize = 33;
  layout.pages = 2;
  layout.points.resize(6);
  layout.owners.resize(33 * 33 * 2, lm::unowned);
  const uint32_t positions[][3] = {{0,15,15},{0,16,15},{0,15,16},{0,16,16},{1,32,32},{1,0,0}};
  for (uint32_t i = 0; i < 6; ++i) {
    const auto &p = positions[i];
    layout.owners[(p[0] * 33 + p[2]) * 33 + p[1]] = i;
  }
  layout.owners[15 * 33 + 14] = 0; // retained padding/donor ownership
  std::vector<b::Result> results(6), probes(5);
  for (uint32_t i = 0; i < results.size(); ++i) {
    results[i].irradiance[0] = {.1f + i, .5f, .25f, std::numeric_limits<float>::quiet_NaN()};
    results[i].irradiance[2] = {-1, .125f, 0, 0};
    results[i].irradiance[8][2] = i == 4 ? 100000.f : -2e-6f;
    results[i].statistics[0] = std::numeric_limits<float>::infinity(); // geometry does not belong here
  }
  probes[0] = results[0];
  probes[3] = results[5];
  const s::Dimensions dimensions{33, 2, 5};
  const uint32_t style[] = {32};
  s::Builder builder(dimensions, 1u << 20);
  builder.add(32, layout, results, probes);
  const auto data = builder.serialize();
  s::validate(data, dimensions, style, 1u << 20);
  const auto dense = lm::pixels(layout, results);
  for (uint32_t page = 0; page < 2; ++page)
    for (uint32_t y = 0; y < 33; ++y) for (uint32_t x = 0; x < 33; ++x) {
      const auto value = s::surface(data, 32, page, x, y);
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
        for (uint32_t channel = 0; channel < 3; ++channel) {
          const auto index = (((page * lm::atlasLayers + coefficient) * 33 + y) * 33 + x) * 4 + channel;
          float largest = 0;
          for (uint32_t c = 0; c < 3; ++c)
            largest = std::max(largest, std::abs(forward::half::decode(dense[index - channel + c])));
          require(std::abs(value[coefficient][channel] - forward::half::decode(dense[index])) <=
                      std::max(2e-7f, largest / 255 + largest * .0005f),
                  "Packed surface SH preserves donors/page boundaries within the shared-exponent error bound");
        }
    }
  for (uint32_t index = 0; index < 5; ++index) {
    const auto value = s::probe(data, 32, index);
    for (uint32_t coefficient = 0; coefficient < 9; ++coefficient)
      for (uint32_t channel = 0; channel < 3; ++channel) {
        float largest = 0;
        for (uint32_t c = 0; c < 3; ++c)
          largest = std::max(largest, std::abs(std::clamp(probes[index].irradiance[coefficient][c], -65000.f, 65000.f)));
        require(std::abs(value[coefficient][channel] - std::clamp(probes[index].irradiance[coefficient][channel], -65000.f, 65000.f)) <=
                    std::max(2e-7f, largest / 255),
                "Packed probe SH preserves signed coefficients within its error bound");
      }
  }
  require(s::probe(data, 97, 0) == s::Coefficients{} && s::probe(data, 32, 5) == s::Coefficients{},
          "Absent style groups and out-of-range probe addresses contribute zero");
  const b::StyleScale on[] = {{32, 1, b::StyleStatus::Ready}};
  const b::StyleScale off[] = {{32, 0, b::StyleStatus::Ready}};
  const Vec4 pageOne{.5f / 33, .5f / 33, 1, 1};
  auto belowPage = pageOne;
  belowPage[2] = std::nextafter(1.f, 0.f);
  const auto onPageOne = s::surfaceIrradiance(data, on, pageOne, {0,0,1});
  require(onPageOne[0] > 0 && onPageOne ==
              s::surfaceIrradiance(data, on, belowPage, {0,0,1}),
          "Interpolated integer page IDs must round to their own page instead of leaking the previous page");
  const Vec4 centre{15.5f/33, 15.5f/33, 0, 1};
  require(s::surfaceIrradiance(data, on, centre, {0,0,1})[0] < 0,
          "Negative SH reconstruction remains signed until base lighting is combined");
  require(s::surfaceIrradiance(data, off, centre, {0,0,1}) == Vec3{},
          "An off style contributes no surface lighting");
  const auto crossing = s::surfaceIrradiance(data, on, {16.f/33,16.f/33,0,1}, {0,0,1});
  Vec3 average{};
  for (uint32_t y : {15u,16u}) for (uint32_t x : {15u,16u}) {
    auto value = s::surfaceIrradiance(data, on, {(x+.5f)/33,(y+.5f)/33,0,1}, {0,0,1});
    for (uint32_t c = 0; c < 3; ++c) average[c] += value[c] * .25f;
  }
  for (uint32_t c = 0; c < 3; ++c)
    require(std::abs(crossing[c] - average[c]) < 1e-6f, "Bilinear filtering crosses sparse tile boundaries continuously");
  {
    const s::Dimensions dimensions{32,1,1};
    const uint32_t allowed[] = {32,97,127};
    auto red = [] {
      s::Coefficients c{}; c[0][0] = 1; c[2][0] = -.25f; return c;
    };
    auto green = [] {
      s::Coefficients c{}; c[0][1] = 2; c[2][1] = -.5f; return c;
    };
    s::Builder indirect(dimensions, 1u<<20), direct(dimensions, 1u<<20), disjoint(dimensions, 1u<<20);
    indirect.addSampled(32, [&](uint32_t, uint32_t x, uint32_t y) {
      return x < 16 && y < 16 ? red() : s::Coefficients{};
    }, [&](uint32_t) { return red(); });
    direct.addSampled(32, [&](uint32_t, uint32_t x, uint32_t y) {
      if (x >= 16 && y >= 16) return green();
      s::Coefficients c{}; if (x == 0 && y == 0) c[0][0] = 3; return c;
    }, [&](uint32_t) { return green(); });
    direct.addSampled(97, [](uint32_t, uint32_t, uint32_t) { return s::Coefficients{}; },
        [](uint32_t) { s::Coefficients c{}; c[0][2] = 4; return c; });
    disjoint.addSampled(32, [&](uint32_t, uint32_t x, uint32_t y) {
      return x >= 16 && y >= 16 ? green() : s::Coefficients{};
    }, [&](uint32_t) { return green(); });
    const auto a = indirect.serialize(), b = direct.serialize(), c = disjoint.serialize();
    const auto aBefore = a, bBefore = b;
    const auto combined = s::sumComponents(a, b, dimensions, allowed, 1u<<20);
    require(combined.has_value(), "Independent direct and indirect groups fit a combined publication");
    const uint32_t unionStyles[] = {32,97};
    s::validate(*combined, dimensions, unionStyles, 1u<<20);
    const auto overlap = s::surface(*combined, 32, 0, 0, 0);
    require(overlap[0][0] == 4 && overlap[2][0] == -.25f,
            "Surface direct and indirect signed SH add exactly once");
    const auto probe = s::probe(*combined, 32, 0);
    require(probe[0][0] == 1 && probe[0][1] == 2 && probe[2][0] == -.25f && probe[2][1] == -.5f &&
                s::probe(*combined, 97, 0)[0][2] == 4 && s::probe(*combined, 127, 0) == s::Coefficients{},
            "Independent probe styles preserve signed sums, membership and known zero");
    require(s::surface(*combined, 32, 0, 31, 31)[0][1] == 2,
            "Merged sparse support includes direct-only tiles");
    require(!s::sumComponents(a, c, dimensions, allowed, std::max(a.size(),c.size()) * 4),
            "Combined storage exhaustion declines the whole replacement");
    require(a == aBefore && b == bBefore, "Combination never mutates either captured component");
    std::stop_source stop; stop.request_stop();
    refused([&] { s::sumComponents(a,b,dimensions,allowed,1u<<20,stop.get_token()); },
            "Cancelled combination exposes no replacement");
    const uint32_t wrongStyles[] = {32};
    refused([&] { s::sumComponents(a,b,dimensions,wrongStyles,1u<<20); },
            "Unexpected style identities reject before publication");
    auto malformed = b; malformed[1] = 999;
    refused([&] { s::sumComponents(a,malformed,dimensions,allowed,1u<<20); },
            "Malformed component payload cannot enter the merged table");
  }
  const uint64_t denseBytes = uint64_t(33) * 33 * 2 * 9 * 3 * sizeof(uint16_t);
  require(builder.bytes() < denseBytes, "Sparse tiles avoid storing unrelated empty atlas space");
  s::Builder tight(dimensions, builder.bytes());
  tight.add(32, layout, results, probes);
  const auto completed = tight.serialize();
  refused([&] { tight.add(97, layout, results, probes); }, "A second component cannot exceed its total encoded budget");
  require(tight.serialize() == completed, "Failed additions preserve all completed style data");
  std::stop_source stop;
  stop.request_stop();
  refused([&] { builder.add(97, layout, results, probes, stop.get_token()); }, "Cancelled additions stop before publication");
  require(builder.serialize() == data, "Cancellation does not partially append a component");
  auto badResults = results;
  badResults[0].irradiance[0][0] = std::numeric_limits<float>::infinity();
  refused([&] { builder.add(97, layout, badResults, probes); }, "Non-finite RGB cannot enter a coefficient record");
  auto badLayout = layout;
  badLayout.owners[0] = UINT32_MAX - 1;
  refused([&] { builder.add(97, badLayout, results, probes); }, "Invalid owner addresses cannot read outside sampled surfaces");
  auto corrupt = [&](auto change) {
    auto copy = data;
    change(copy);
    refused([&] { s::validate(copy, dimensions, style, 1u << 20); }, "Malformed cache data is refused before GPU upload");
  };
  corrupt([](auto &v) { v.pop_back(); });
  corrupt([](auto &v) { v[1] = 1; });
  corrupt([](auto &v) { v[4] = UINT32_MAX; });
  corrupt([](auto &v) { v[12 + 1] = UINT32_MAX; });
  corrupt([](auto &v) { v[12 + 4] = UINT32_MAX; });
  corrupt([](auto &v) { v[v[13]] = UINT32_MAX; });
  corrupt([](auto &v) { v[v[15]] = 0xffffffffu; });
  corrupt([](auto &v) { v[v[15]] = 1u << 27; });
  refused([&] { s::validate(data, dimensions, style, data.size() * sizeof(uint32_t) - 1); }, "Cache data obeys the device/storage byte limit");
  s::Builder ordered(dimensions, 2u << 20), reversed(dimensions, 2u << 20);
  const std::vector<b::Result> blackResults(6), blackProbes(5);
  ordered.add(32, layout, results, probes);
  ordered.add(97, layout, blackResults, blackProbes);
  reversed.add(97, layout, blackResults, blackProbes);
  reversed.add(32, layout, results, probes);
  require(ordered.serialize() == reversed.serialize(), "Storage identity is independent of component completion order");
  const uint32_t styles[] = {32,97};
  s::validate(ordered.serialize(), dimensions, styles, 2u << 20);
  lm::Layout empty;
  empty.pageSize = 1024;
  empty.pages = 4;
  empty.owners.resize(4 * 1024 * 1024, lm::unowned);
  s::Builder large({1024,4,0}, 128u << 10);
  large.add(32, empty, {}, {});
  const auto emptyData = large.serialize();
  require(emptyData[16] == 0 && large.bytes() < (128u << 10),
          "An empty four-megatexel style stores only sparse lookup words, not a directional atlas");
  std::printf("Sparse style SH: %llu bytes vs %llu dense RGB-half bytes; empty4Mfield %llu bytes\n",
              static_cast<unsigned long long>(builder.bytes()), static_cast<unsigned long long>(denseBytes),
              static_cast<unsigned long long>(large.bytes()));
}
