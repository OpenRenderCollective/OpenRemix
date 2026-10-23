#pragma once
#include "forward/mixed/directional_capture.h"
#include <algorithm>
#include <limits>

void directionalCaptureTests() {
  namespace mixed = remix::forward::mixed;
  namespace lm = remix::forward::lightmap;
  lm::Layout layout;
  layout.pageSize = 8; layout.pages = 1;
  layout.charts = {{0, 1, 1, 4, 1}, {0, 1, 3, 2, 1}};
  layout.pointCharts = {0, 1, 0, 0, 1};
  layout.receiverSamples = {1, 1, 0, 1, 1}; // a seam donor must remain in the field
  layout.points.resize(5);
  layout.owners.assign(64, lm::unowned);
  layout.owners[9] = 0; layout.owners[10] = 2; layout.owners[12] = 3;
  layout.owners[25] = 1; layout.owners[26] = 4;
  std::vector<mixed::DirectionalCapture::Link> links;
  for (uint32_t light = 0; light < 7; ++light) links.push_back({0, light});
  links.push_back({1, 6});
  auto value = [](uint32_t point, uint32_t light) {
    remix::forward::bake::VisibilityResult result;
    result.visibility = light == 6 ? 0.f : 1.f;
    for (uint32_t c = 0; c < 9; ++c)
      result.irradiance[c] = light == 5 ? 0.f : float(int(point) - 2) * float(c + 1) / 16 + float(light) / 32;
    return result;
  };
  mixed::DirectionalCapture capture(layout, 8, links, 1024);
  const auto resident = capture.stagingBytes();
  for (uint32_t field = 0; field < links.size(); ++field) {
    require(!capture.done(), "A manifest is incomplete until every declared field commits");
    const auto count = capture.begin();
    const auto tasks = capture.batch(0, count);
    std::vector<remix::forward::bake::VisibilityResult> values;
    for (const auto &task : tasks) {
      require(task.light == links[field].light && task.channel == field &&
                  layout.pointCharts[task.point] == links[field].chart,
              "Field-major tasks preserve captured light, chart and unbounded output identity");
      values.push_back(value(task.point, task.light));
    }
    rejects([&] { capture.finish(); });
    rejects([&] { capture.words(); });
    auto invalid = values;
    invalid.back().irradiance[8] = std::numeric_limits<float>::quiet_NaN();
    rejects([&] { capture.observe(0, invalid); });
    capture.observe(count - 1, std::span(values).last(1));
    rejects([&] { capture.observe(count - 1, std::span(values).last(1)); });
    capture.observe(0, std::span(values).first(count - 1));
    require(capture.finish() && capture.stagingBytes() == resident,
            "Complete fields commit and release staging before the next light is captured");
  }
  require(capture.done(), "Seven overlaps plus a second chart complete without an RGBA owner cap");
  const auto words = capture.words();
  mixed::DirectionalFields::validate(words, 2, 8, mixed::DirectionalFields::maximumBytes);
  for (const auto &link : links) {
    const auto &r = layout.charts[link.chart];
    for (uint32_t x = 0; x < r.width; ++x) {
      const auto field = mixed::DirectionalFields::sample(words, link.chart, link.light, x, 0);
      require(bool(field), "Known-black and scalar-blocked fields retain explicit ownership");
      const auto owner = layout.owners[r.y * 8 + r.x + x];
      for (uint32_t c = 0; c < 9; ++c)
        require((*field)[c] == remix::forward::half::encode(owner == lm::unowned ? 0.f : value(owner, link.light).irradiance[c]),
                "Signed SH, seam donors, isolated padding and light identity survive collection");
    }
  }
  require(!mixed::DirectionalFields::sample(words, 0, 7, 0, 0), "Undeclared coverage stays unknown");
  {
    mixed::DirectionalCapture windowed(layout, 8, links, 4096);
    uint32_t windows = 0;
    while (!windowed.done()) {
      const auto count = windowed.beginWindow(9, 4);
      require(count <= 9 && windowed.window().size() <= 4,
              "Windows obey their point target and field limit");
      const auto tasks = windowed.batch(0, count);
      std::vector<uint32_t> pending;
      for (uint32_t i = 0; i < count; ++i) {
        require(layout.pointCharts[tasks[i].point] == links[tasks[i].channel].chart &&
                    tasks[i].light == links[tasks[i].channel].light,
                "Windows preserve unbounded field identities across chart boundaries");
        if (i & 1) pending.push_back(i);
        else {
          const auto supplied = value(tasks[i].point, tasks[i].light);
          windowed.observe(i, {&supplied, 1}); // retained samples can leave sparse hardware receipts
        }
      }
      std::reverse(pending.begin(), pending.end());
      const auto sparse = windowed.batch(pending);
      std::vector<remix::forward::bake::VisibilityResult> values;
      for (const auto &task : sparse) values.push_back(value(task.point, task.light));
      if (pending.size() > 1) {
        auto duplicate = pending; duplicate[1] = duplicate[0];
        rejects([&] { windowed.observe(duplicate, values); });
      }
      auto invalid = values;
      invalid.back().irradiance[8] = std::numeric_limits<float>::quiet_NaN();
      rejects([&] { windowed.observe(pending, invalid); });
      rejects([&] { windowed.finish(); });
      windowed.observe(pending, values);
      rejects([&] { windowed.observe(pending, values); });
      require(windowed.finish() && windowed.stagingBytes() == resident,
              "Complete windows release staging without accumulating all-map coefficients");
      ++windows;
    }
    require(windows == 3 && windowed.words() == words,
            "Mixed sparse receipts and multi-field windows preserve every encoded half bit");
  }
  {
    mixed::DirectionalCapture tight(layout, 8, links, 300);
    require(tight.beginWindow(100, 8) == 3 && tight.window().size() == 1,
            "Staging pressure shrinks a window instead of discarding fields");
    mixed::DirectionalCapture large(layout, 8, links, 4096);
    require(large.beginWindow(1) == 3 && large.window().size() == 1,
            "One large field can exceed the soft point target while retaining hard staging bounds");
    auto withEmpty = layout;
    withEmpty.charts.push_back({0, 7, 7, 1, 1});
    const mixed::DirectionalCapture::Link emptyLinks[] = {{2, 0}, {0, 0}, {2, 1}};
    mixed::DirectionalCapture zeros(withEmpty, 8, emptyLinks, 4096);
    require(zeros.beginWindow(8) == 3 && zeros.window().size() == 3,
            "Empty fields can share a window with real receivers");
    const auto tasks = zeros.batch(0, 3);
    const uint32_t reversed[] = {2, 1, 0};
    const auto sparse = zeros.batch(reversed);
    require(tasks.front().channel == 1 && sparse.front().point == tasks.back().point,
            "Zero-length field boundaries cannot steal another field's receipt");
    std::vector<remix::forward::bake::VisibilityResult> values(3);
    zeros.observe(reversed, values);
    std::stop_source cancelled; cancelled.request_stop();
    rejects([&] { zeros.finish(cancelled.get_token()); });
    rejects([&] { zeros.words(); });
    require(zeros.finish() && zeros.done(), "Cancellation cannot advance or corrupt a window");
  }
  auto duplicate = links; duplicate.push_back(links[0]);
  {
    std::vector<mixed::DirectionalCapture::Link> many;
    for (uint32_t light = 0; light < 512; ++light) many.push_back({0, light});
    mixed::DirectionalCapture capped(layout, 512, many, 65536);
    mixed::DirectionalCapture tightDirectory(layout, 512, many, 4300);
    require(tightDirectory.beginWindow(100) == 3 && tightDirectory.window().size() == 1,
            "A small staging budget need not reserve the maximum window directory");
    uint32_t windows = 0;
    while (!capped.done()) {
      const auto count = capped.beginWindow(UINT32_MAX);
      require(capped.window().size() == 256 && count == 768 &&
                  capped.batch(count - 1, 1).front().channel == (windows + 1) * 256 - 1,
              "Large manifests split at the hard field limit without narrowing global field tags");
      std::vector<remix::forward::bake::VisibilityResult> zero(count);
      capped.observe(0, zero);
      require(capped.finish(), "A bounded large-overlap window encodes every declared owner");
      ++windows;
    }
    require(windows == 2 && capped.words()[4] == 512,
            "Window boundaries cannot silently truncate large overlap manifests");
  }
  rejects([&] { mixed::DirectionalCapture bad(layout, 8, duplicate, 1024); });
  mixed::DirectionalCapture tiny(layout, 8, links, resident);
  rejects([&] { tiny.begin(); });
  auto wrongOwner = layout; wrongOwner.owners[9] = 1;
  rejects([&] { mixed::DirectionalCapture bad(wrongOwner, 8, links, 1024); });
  std::stop_source stop; stop.request_stop();
  rejects([&] { mixed::DirectionalCapture bad(layout, 8, links, 1024, 1024, stop.get_token()); });
  const mixed::DirectionalCapture::Link black[] = {{0, 0}, {0, 1}};
  mixed::DirectionalCapture limited(layout, 8, black, 1024, 100);
  for (uint32_t i = 0; i < 2; ++i) {
    const auto count = limited.begin();
    std::vector<remix::forward::bake::VisibilityResult> zero(count);
    limited.observe(0, zero);
    require(limited.finish() == (i == 0), "Storage exhaustion declines a whole replacement after partial progress");
    const auto usage = limited.storageUsage();
    require(usage.rawFields + usage.tiledFields == 1 && usage.encodedBytes <= 100 && usage.rawBytes > 0,
            "Storage diagnostics count accepted fields only, including after exhaustion");
  }
  rejects([&] { limited.words(); });
  mixed::DirectionalCapture limitedWindow(layout, 8, black, 4096, 100);
  const auto limitedCount = limitedWindow.beginWindow(10);
  std::vector<remix::forward::bake::VisibilityResult> zeroWindow(limitedCount);
  limitedWindow.observe(0, zeroWindow);
  require(!limitedWindow.finish(), "Encoded exhaustion refuses an entire multi-field window");
  rejects([&] { limitedWindow.words(); });
  mixed::DirectionalCapture empty(layout, 8, {}, 0);
  require(empty.done() && empty.stagingBytes() == 0,
          "An empty capture manifest allocates no receiver index or sample staging");
  mixed::DirectionalFields::validate(empty.words(), 2, 8, mixed::DirectionalFields::maximumBytes);
}
