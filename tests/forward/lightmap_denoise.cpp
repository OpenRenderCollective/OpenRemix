#include "forward/lightmap/denoise.h"
#include "forward/lightmap/denoise_chart.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <stdexcept>

using namespace remix::forward;
namespace {
  void require(bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  lightmap::Layout squareLayout() {
    lightmap::Layout layout;
    layout.pageSize = 512;
    layout.pages = 1;
    layout.charts = {{0, 0, 0, 512, 512}};
    const size_t count = 512 * 512;
    layout.points.resize(count);
    layout.pointCharts.resize(count);
    layout.addresses.resize(count);
    layout.owners.resize(count);
    std::iota(layout.addresses.begin(), layout.addresses.end(), 0u);
    std::iota(layout.owners.begin(), layout.owners.end(), 0u);
    return layout;
  }
  void chartPacking() {
    using namespace lightmap::denoising;
    auto layout = squareLayout();
    const size_t count = layout.points.size();
    bake::Result source;
    source.irradiance[0] = {.5f, .5f, .5f, 7};
    for (uint32_t coefficient = 1; coefficient < 9; ++coefficient)
      source.irradiance[coefficient] = {.01f * coefficient, -.02f * coefficient, .03f * coefficient, 7};
    source.moments[0] = {1, 2, 3, 4};
    source.statistics = {5, 6, 7, 8};
    std::vector<bake::Result> values(count, source);
    Chart chart(layout, 0, values, layout.owners, {});
    require(chart.fieldsPerBatch() == 2 && batchFields(16u << 20) == 1,
            "Large chart fields split into bounded batches, with a single-field fallback");
    require(chart.hostBytes() == count * 3 * sizeof(float) * 4,
            "Chart scratch holds original/filtered L0 and only the bounded field batch");
    uint32_t batches = 0;
    for (uint32_t first = 0; first < 9; first += chart.fieldsPerBatch()) {
      const auto fields = std::min(chart.fieldsPerBatch(), 9 - first);
      auto packet = chart.pack(first, fields);
      require(packet.size_bytes() <= batchBudgetBytes, "Batches respect the image transfer budget");
      if (!first)
        std::fill_n(packet.begin(), count * 3, 1.f); // simulate filtered L0 doubling
      chart.apply(first, fields, packet);
      ++batches;
    }
    chart.finish();
    require(batches == 5, "The last partial coefficient batch is processed");
    for (const auto point : {size_t(0), count / 2, count - 1}) {
      require(values[point].moments == source.moments && values[point].statistics == source.statistics,
              "Chunk reconstruction preserves transport metadata");
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient) {
        require(values[point].irradiance[coefficient][3] == 7, "Chunk reconstruction preserves alpha");
        for (uint32_t channel = 0; channel < 3; ++channel)
          require(std::abs(values[point].irradiance[coefficient][channel] -
                           2 * source.irradiance[coefficient][channel]) < 1e-6f,
                  "Every batch normalizes with original L0 and restores signed bands with filtered L0");
      }
    }
  }
  void largeBatch() {
    auto layout = squareLayout();
    std::vector<bake::Result> values(layout.points.size());
    for (uint32_t i = 0; i < values.size(); ++i) {
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient) {
        values[i].irradiance[coefficient][3] = 7;
        for (uint32_t c = 0; c < 3; ++c) {
          const float noise = float((i * 1664525u + coefficient * 7919u + c * 1171u) % 1009) / 1008.f - .5f;
          values[i].irradiance[coefficient][c] =
              coefficient ? (coefficient % 2 ? -.2f : .2f) + noise * .07f : 1 + noise * .5f;
        }
      }
      values[i].statistics = {1, 2, 3, 4};
    }
    const auto statistics = lightmap::denoise(layout, values);
    require(statistics.batches == 5 && statistics.executions == 9 && statistics.preparations == 9 &&
                statistics.constantFields == 0,
            "Real OIDN execution handles all full coefficient batches and the final single-field batch");
    const size_t fieldBytes = layout.points.size() * 3 * sizeof(float);
    require(statistics.imageBufferBytes == 2 * fieldBytes && statistics.peakHostBytes == 4 * fieldBytes &&
                statistics.transferBytes == 2 * 9 * fieldBytes,
            "Large OIDN chart transfers and scratch stay within the bounded batch plan");
    std::array<double, 9> energy{};
    for (const auto &value : values) {
      require(value.statistics == Vec4{1, 2, 3, 4}, "Large batches preserve transport metadata");
      for (uint32_t coefficient = 0; coefficient < 9; ++coefficient) {
        require(value.irradiance[coefficient][3] == 7, "Large batches preserve alpha");
        for (uint32_t c = 0; c < 3; ++c) {
          require(std::isfinite(value.irradiance[coefficient][c]), "Large batches return finite SH");
          energy[coefficient] += value.irradiance[coefficient][c];
        }
      }
    }
    for (uint32_t coefficient = 0; coefficient < 9; ++coefficient) {
      const float expected = coefficient ? (coefficient % 2 ? -.2f : .2f) : 1.f;
      require(std::abs(energy[coefficient] / (values.size() * 3) - expected) < .04,
              "Large batches retain mean HDR energy and each signed directional band");
    }
    std::printf("OIDN large chart: %u executions in %u bounded batches, %.3f s\n", statistics.executions,
                statistics.batches, statistics.seconds);
  }
} // namespace
int main() try {
  chartPacking();
  if (!lightmap::denoiserVersion()) {
    bool unavailable = false;
    try {
      lightmap::denoise({}, {});
    } catch (const std::runtime_error &) {
      unavailable = true;
    }
    require(unavailable, "An unavailable denoiser must reject explicit filtering requests");
    std::puts("Optional OIDN disabled: explicit filtering requests are rejected");
    return 0;
  }
  largeBatch();
  lightmap::Layout layout;
  layout.pageSize = 128;
  layout.pages = 1;
  // Equal shapes are deliberately separated by another shape in authored order.
  layout.charts = {{0, 0, 0, 64, 64}, {0, 64, 0, 64, 64}, {0, 0, 64, 31, 17}, {0, 64, 64, 64, 64}};
  layout.owners.resize(128 * 128, lightmap::unowned);
  std::vector<bake::Result> values;
  for (uint32_t chart = 0; chart < layout.charts.size(); ++chart)
    for (uint32_t y = 0; y < layout.charts[chart].height; ++y)
      for (uint32_t x = 0; x < layout.charts[chart].width; ++x) {
        const uint32_t index = uint32_t(values.size());
        const auto &allocation = layout.charts[chart];
        const auto address = (y + allocation.y) * 128 + allocation.x + x;
        layout.owners[address] = index;
        layout.addresses.push_back(address);
        layout.pointCharts.push_back(chart);
        layout.points.push_back({});
        bake::Result sample;
        // Deliberately noisy constant lighting next to a completely dark chart.
        const float noise = float((x * 1664525u + y * 1013904223u + chart * 7919u) % 1009) / 1008.f - .5f;
        for (uint32_t c = 0; c < 3; ++c) {
          sample.irradiance[0][c] = chart == 1 ? 0 : 1 + noise * .8f;
          for (uint32_t coefficient = 1; coefficient < 9; ++coefficient)
            sample.irradiance[coefficient][c] = sample.irradiance[0][c] * (coefficient % 2 ? -.3f : .2f);
          if (chart != 1)
            for (uint32_t coefficient : {1u, 3u, 5u})
              sample.irradiance[coefficient][c] +=
                  sample.irradiance[0][c] * .3f * (float((x * 214013u + y * 2531011u) % 1009) / 1008.f - .5f);
        }
        sample.irradiance[0][3] = 7;
        sample.moments[0] = {1, 2, 3, 4};
        sample.statistics = {5, 6, 7, 8};
        values.push_back(sample);
      }
  const auto raw = values;
  std::vector<std::array<uint32_t, 2>> progress;
  const auto statistics = lightmap::denoise(layout, values, {}, [&](uint32_t completed, uint32_t total) {
    progress.push_back({completed, total});
  });
  double before = 0, after = 0, directionBefore = 0, directionAfter = 0;
  for (uint32_t i = 0; i < values.size(); ++i) {
    const auto &sample = values[i];
    require(sample.moments == raw[i].moments && sample.statistics == raw[i].statistics,
            "Denoising must preserve transport metadata");
    for (uint32_t coefficient = 0; coefficient < 9; ++coefficient) {
      require(sample.irradiance[coefficient][3] == raw[i].irradiance[coefficient][3],
              "Denoising must preserve all SH alpha components");
      for (uint32_t c = 0; c < 3; ++c) {
        require(std::isfinite(sample.irradiance[coefficient][c]), "Finite denoised SH");
        if (layout.pointCharts[i] == 1)
          require(std::abs(sample.irradiance[coefficient][c]) < .0001f,
                  "Denoising must not leak lighting between packed charts");
        else if (coefficient && coefficient != 1 && coefficient != 3 && coefficient != 5) {
          require(std::abs(sample.irradiance[coefficient][c] -
                           sample.irradiance[0][c] * (coefficient % 2 ? -.3f : .2f)) < .025f,
                  "Denoising must preserve signed SH direction relative to filtered L0");
        }
        if (coefficient == 3 || coefficient == 5)
          require(std::abs(sample.irradiance[coefficient][c] - sample.irradiance[1][c]) < .000001f,
                  "Reusing a committed model must read each newly uploaded field without stale output");
      }
    }
    if (layout.pointCharts[i] != 1) {
      before += std::pow(raw[i].irradiance[0][0] - 1, 2);
      after += std::pow(sample.irradiance[0][0] - 1, 2);
      directionBefore += std::pow(raw[i].irradiance[1][0] / raw[i].irradiance[0][0] + .3f, 2);
      directionAfter += std::pow(sample.irradiance[1][0] / std::max(sample.irradiance[0][0], 1e-6f) + .3f, 2);
    }
  }
  require(after < before * .5, "Denoising must materially reduce error against known clean lighting");
  require(directionAfter < directionBefore * .5, "Directional denoising must reduce normalized SH error");
  std::stop_source cancellation;
  cancellation.request_stop();
  bool stopped = false;
  try {
    lightmap::denoise(layout, values, cancellation.get_token());
  } catch (const std::runtime_error &) {
    stopped = true;
  }
  require(stopped, "Cancelled denoising must stop before publication");
  require(statistics.charts == 4 && statistics.texels == 12815, "Chart denoising accounting");
  require(progress.size() == 5, "Denoising reports start and completion of each chart");
  for (uint32_t i = 0; i < progress.size(); ++i)
    require(progress[i] == std::array<uint32_t, 2>{i, 4},
            "Denoising progress is monotonic with a stable total");
  require(statistics.executions == 12 && statistics.preparations == 8 && statistics.constantFields == 24,
          "Per-band models are prepared only for changed image shapes, not every chart");
  require(statistics.batches == 3, "Each nonconstant small chart needs only one upload/fence/readback batch");
  require(statistics.monitoredExecutions == (statistics.deviceType == 1 ? 12u : 0u),
          "Small GPU fields avoid internal progress callbacks; CPU filters retain in-flight cancellation");
  require(statistics.peakHostBytes == 64u * 64 * 3 * 11 * sizeof(float) &&
              statistics.imageBufferBytes == 64u * 64 * 3 * 9 * sizeof(float),
          "Host images and reusable device buffer are bounded by the largest chart");
  std::printf("OIDN %u device type %d: chart isolation, signed SH, metadata and cancellation passed; "
              "L0 RMSE %.5f -> %.5f, directional RMSE %.5f -> %.5f, %.3f s\n",
              lightmap::denoiserVersion(), statistics.deviceType, std::sqrt(before / 8719),
              std::sqrt(after / 8719), std::sqrt(directionBefore / 8719), std::sqrt(directionAfter / 8719),
              statistics.seconds);
} catch (const std::exception &error) {
  std::fprintf(stderr, "Lightmap denoising test failed: %s\n", error.what());
  return 1;
}
