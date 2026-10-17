#include "forward/lightmap/denoise.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using namespace remix::forward;
namespace {
  void require(bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  }
} // namespace
int main() try {
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
  lightmap::Layout layout;
  layout.pageSize = 128;
  layout.pages = 1;
  // Equal shapes are deliberately separated by another shape in authored order.
  layout.charts = {{0, 0, 0, 64, 64}, {0, 64, 0, 64, 64}, {0, 0, 64, 32, 32}, {0, 64, 64, 64, 64}};
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
  require(statistics.charts == 4 && statistics.texels == 13312, "Chart denoising accounting");
  require(progress.size() == 5, "Denoising reports start and completion of each chart");
  for (uint32_t i = 0; i < progress.size(); ++i)
    require(progress[i] == std::array<uint32_t, 2>{i, 4},
            "Denoising progress is monotonic with a stable total");
  require(statistics.executions == 12 && statistics.preparations == 4 && statistics.constantFields == 24,
          "HDR and directional models are prepared only for changed image shapes, not every field");
  require(statistics.monitoredExecutions == (statistics.deviceType == 1 ? 12u : 0u),
          "Small GPU fields avoid internal progress callbacks; CPU filters retain in-flight cancellation");
  require(statistics.peakHostBytes == 64u * 64 * 3 * 3 * sizeof(float) &&
              statistics.imageBufferBytes == 64u * 64 * 3 * sizeof(float),
          "Host images and reusable device buffer are bounded by the largest chart");
  std::printf("OIDN %u device type %d: chart isolation, signed SH, metadata and cancellation passed; "
              "L0 RMSE %.5f -> %.5f, directional RMSE %.5f -> %.5f, %.3f s\n",
              lightmap::denoiserVersion(), statistics.deviceType, std::sqrt(before / 9216),
              std::sqrt(after / 9216), std::sqrt(directionBefore / 9216), std::sqrt(directionAfter / 9216),
              statistics.seconds);
} catch (const std::exception &error) {
  std::fprintf(stderr, "Lightmap denoising test failed: %s\n", error.what());
  return 1;
}
