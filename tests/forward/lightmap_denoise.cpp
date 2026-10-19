#include "denoise_benchmark.h"
#include "forward/lightmap/denoise.h"
#include "forward/lightmap/denoise_chart.h"
#include "forward/lightmap/denoise_cpu.h"
#include "forward/lightmap/denoise_device.h"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>

using namespace remix::forward;
namespace {
  void require(bool condition, const char *message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  void queueFailures() {
    std::atomic_uint retired = 0;
    std::barrier ready(2);
    bool failed = false;
    try {
      lightmap::denoising::cpuQueues(2, {}, [&](uint32_t index, std::stop_token stop) {
        struct Retire {
          std::atomic_uint &count;
          ~Retire() { ++count; }
        } retire{retired};
        ready.arrive_and_wait();
        if (!index)
          throw std::runtime_error("Expected queue failure");
        while (!stop.stop_requested())
          std::this_thread::yield();
      });
    } catch (const std::runtime_error &) {
      failed = true;
    }
    require(failed && retired == 2, "Queue failures cancel and join every peer before returning");
  }
#ifdef REMIX_OIDN
  void parallelCharts(const lightmap::Layout &layout, const std::vector<bake::Result> &source) {
    using namespace lightmap;
    using namespace lightmap::denoising;
    std::vector<uint32_t> order(layout.charts.size());
    std::iota(order.begin(), order.end(), 0u);
    std::vector<std::vector<uint32_t>> points(layout.charts.size());
    for (uint32_t i = 0; i < source.size(); ++i)
      points[layout.pointCharts[i]].push_back(i);
    auto serial = source, parallel = source;
    DenoiseStatistics a, b;
    filterChartQueues(layout, serial, order, points, 8u << 20, {FilterDevice::Cpu}, HostTransfers::Pageable,
                      FilterDispatch::Ordinary, {}, a, {});
    std::vector<uint32_t> progress;
    filterChartQueues(
        layout, parallel, order, points, 8u << 20, {FilterDevice::Cpu, 4, 2}, HostTransfers::Pageable,
        FilterDispatch::Ordinary, {}, b, [&](uint32_t done, uint32_t total) {
          require(total == layout.charts.size(), "Parallel chart progress retains its global total");
          progress.push_back(done);
        });
    for (size_t i = 0; i < source.size(); ++i)
      require(serial[i].irradiance == parallel[i].irradiance && serial[i].moments == parallel[i].moments &&
                  serial[i].statistics == parallel[i].statistics,
              "Independent CPU chart queues preserve serial fields and metadata exactly");
    require(a.charts == b.charts && a.executions == b.executions && a.texels == b.texels && b.cpuQueues == 4,
            "Parallel chart counters aggregate actual work");
    for (uint32_t i = 0; i < progress.size(); ++i)
      require(progress[i] == i, "Parallel chart progress remains serialized and monotonic");
    require(progress.size() == layout.charts.size() + 1, "Every parallel chart reports completion");
    bool failed = false;
    try {
      filterChartQueues(layout, parallel, order, points, 8u << 20, {FilterDevice::Cpu, 4, 2},
                        HostTransfers::Pageable, FilterDispatch::Ordinary, {}, b,
                        [](uint32_t done, uint32_t) {
                          if (done)
                            throw std::runtime_error("Expected progress failure");
                        });
    } catch (const std::runtime_error &) {
      failed = true;
    }
    require(failed, "Parallel filtering propagates callback failures after draining work");
    std::stop_source cancellation;
    bool stopped = false;
    try {
      filterChartQueues(layout, parallel, order, points, 8u << 20, {FilterDevice::Cpu, 4, 2},
                        HostTransfers::Pageable, FilterDispatch::Ordinary, cancellation.get_token(), b,
                        [&](uint32_t done, uint32_t) {
                          if (done)
                            cancellation.request_stop();
                        });
    } catch (const std::runtime_error &) {
      stopped = true;
    }
    require(stopped, "External cancellation interrupts and drains independent chart queues");
  }
#endif
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
#ifdef REMIX_OIDN
  void deviceCalibrationPreservesInput() {
    lightmap::Layout layout;
    layout.pageSize = 256;
    layout.pages = 1;
    layout.owners.resize(256 * 256, lightmap::unowned);
    std::vector<bake::Result> values;
    std::vector<std::vector<uint32_t>> points(4096);
    for (uint32_t chart = 0; chart < 4096; ++chart) {
      layout.charts.push_back({0, (chart % 64) * 4, (chart / 64) * 4, 4, 4});
      const auto &a = layout.charts.back();
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
          const uint32_t index = uint32_t(values.size()), address = (a.y + y) * 256 + a.x + x;
          layout.owners[address] = index;
          layout.addresses.push_back(address);
          layout.pointCharts.push_back(chart);
          layout.points.push_back({});
          points[chart].push_back(index);
          bake::Result value;
          for (uint32_t c = 0; c < 9; ++c)
            value.irradiance[c] = {.2f + .01f * x, .1f + .02f * y, .3f + .01f * (x + y), 7};
          value.moments[0] = {1, 2, 3, 4};
          values.push_back(value);
        }
    }
    const auto before = values;
    lightmap::DenoiseStatistics statistics;
    (void)lightmap::denoising::chooseDevice(layout, values, points, 16, 16 * 16 * 3 * 9 * sizeof(float),
        lightmap::HostTransfers::Pinned, lightmap::FilterDispatch::Automatic, {}, statistics);
    require(std::memcmp(values.data(), before.data(), values.size() * sizeof(bake::Result)) == 0,
            "Device calibration never applies trial filtering to captured lighting or metadata");
    require(statistics.executions == 0 && statistics.charts == 0 && statistics.texels == 0,
            "Calibration work stays separate from production chart and filter counters");
  }
#endif
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
  void paddedCharts() {
    lightmap::Layout layout;
    layout.pageSize = 128;
    layout.pages = 1;
    layout.charts = {{0, 0, 0, 17, 31}, {0, 32, 0, 31, 17}, {0, 64, 0, 25, 29}};
    layout.owners.resize(128 * 128, lightmap::unowned);
    std::vector<bake::Result> values;
    for (uint32_t chart = 0; chart < layout.charts.size(); ++chart) {
      const auto &a = layout.charts[chart];
      for (uint32_t y = 0; y < a.height; ++y)
        for (uint32_t x = 0; x < a.width; ++x) {
          const auto point = uint32_t(values.size()), address = (a.y + y) * 128 + a.x + x;
          layout.points.push_back({});
          layout.pointCharts.push_back(chart);
          layout.addresses.push_back(address);
          layout.owners[address] = point;
          bake::Result value;
          const float noise = float((x * 1664525u + y * 1013904223u) % 1009) / 1008.f - .5f;
          for (uint32_t c = 0; c < 3; ++c)
            for (uint32_t band = 0; band < 9; ++band) {
              const float level = float(c + 1) * (band ? -.1f : 1.f);
              value.irradiance[band][c] = chart == 2 ? level : level + noise * .05f;
              value.irradiance[band][3] = 7;
            }
          value.statistics = {float(point), 2, 3, 4};
          values.push_back(value);
        }
    }
    const auto original = values;
    std::vector<uint32_t> firstPoints(17 * 31);
    std::iota(firstPoints.begin(), firstPoints.end(), 0u);
    lightmap::denoising::Chart image(layout, 0, values, firstPoints, {}, 16);
    const auto packet = image.pack(0, 9);
    require(image.width() == 32 && image.height() == 32,
            "Temporary denoising images pad independently of original chart bounds");
    for (uint32_t band = 0; band < 9; ++band)
      for (uint32_t y = 0; y < 32; ++y)
        for (uint32_t x = 0; x < 32; ++x)
          if (x >= 17 || y >= 31)
            for (uint32_t c = 0; c < 3; ++c)
              require(packet[(band * 32 * 32 + y * 32 + x) * 3 + c] == (band ? -1.f : 0.f),
                      "Synthetic boundaries are zero after HDR and signed network transforms");
    const auto statistics = lightmap::denoise(layout, values);
    // The supported OIDN 2.5 network uses the same 32x32 tensor for all three
    // shapes. A future unknown OIDN version deliberately keeps native shapes.
    if (lightmap::denoiserVersion() >= 20500 && lightmap::denoiserVersion() < 20600)
      require(statistics.preparations == 9 && statistics.synchronizations == 1,
              "Unequal charts sharing network padding reuse bindings and one bounded queue");
    require(statistics.executions == 18 && statistics.constantFields == 9,
            "Padding never turns real constant lighting into a nonconstant field");
    for (uint32_t point = 0; point < values.size(); ++point) {
      require(values[point].statistics == original[point].statistics,
              "Padded image reconstruction preserves transport metadata");
      if (layout.pointCharts[point] == 2)
        require(values[point].irradiance == original[point].irradiance,
                "All constant RGB SH bands survive padding and cropping exactly");
    }
    require(lightmap::denoising::alignedExtent(17, 16) == 32,
            "Image padding rounds up, independently of atlas placement");
    bool rejected = false;
    try {
      lightmap::denoising::alignedExtent(std::numeric_limits<uint32_t>::max(), 16);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    require(rejected, "Padded image dimensions cannot wrap");
  }
  void queuedCharts(uint32_t side, uint32_t count, bool mixedMasks = false) {
    lightmap::Layout layout;
    layout.pageSize = 512;
    layout.pages = 1;
    layout.owners.resize(512 * 512, lightmap::unowned);
    std::vector<bake::Result> values;
    for (uint32_t chart = 0; chart < count; ++chart) {
      const uint32_t originX = (chart % 8) * side, originY = (chart / 8) * side;
      layout.charts.push_back({0, originX, originY, side, side});
      for (uint32_t y = 0; y < side; ++y)
        for (uint32_t x = 0; x < side; ++x) {
          const auto index = uint32_t(values.size()), address = (originY + y) * 512 + originX + x;
          layout.points.push_back({});
          layout.pointCharts.push_back(chart);
          layout.addresses.push_back(address);
          layout.owners[address] = index;
          bake::Result value;
          const float noise = mixedMasks && chart % 7 == 0 ? 0.f :
              float((x * 1664525u + y * 1013904223u + chart * 7919u) % 1009) / 1008.f - .5f;
          for (uint32_t c = 0; c < 3; ++c) {
            value.irradiance[0][c] = 1 + chart * .1f + noise * .3f;
            value.irradiance[1][c] = value.irradiance[0][c] *
                (.2f + (mixedMasks && chart % 5 == 0 ? 0.f : noise * .1f));
          }
          value.irradiance[0][3] = float(chart) + .5f;
          value.statistics = {float(index), 2, 3, 4};
          values.push_back(value);
        }
    }
    const auto original = values;
    if (side == 16) {
      auto invalid = original;
      invalid[size_t(side) * side].irradiance[1][0] = std::numeric_limits<float>::quiet_NaN();
      bool rejected = false;
      try {
        lightmap::denoise(layout, invalid);
      } catch (const std::invalid_argument &) {
        rejected = true;
      }
      require(rejected, "Invalid later chart data cannot publish deferred or queued host transfers");
    }
    if (side == 16) {
      auto interrupted = original;
      std::stop_source stop;
      bool cancelled = false;
      try {
        lightmap::denoise(layout, interrupted, stop.get_token(), [&](uint32_t completed, uint32_t) {
          if (completed) stop.request_stop();
        });
      } catch (const std::runtime_error &) { cancelled = stop.stop_requested(); }
      require(cancelled, "Cancellation after queued filtering retires work before releasing graph resources");
    }
    const auto statistics = lightmap::denoise(layout, values);
    auto pageable = original;
    lightmap::denoise(layout, pageable, {}, {}, lightmap::HostTransfers::Pageable, lightmap::FilterDispatch::Ordinary);
    float error = 0;
    for (size_t point = 0; point < values.size(); ++point)
      for (uint32_t band = 0; band < 9; ++band)
        for (uint32_t c = 0; c < 3; ++c)
          error = std::max(error, std::abs(values[point].irradiance[band][c] - pageable[point].irradiance[band][c]));
    require(error < .0001f, "Graph packets preserve ordinary output across changing charts, byte/count flushes and tails");
    std::printf("OIDN graph queue %u x %ux%u: %u captures, %u launches, %u fallbacks, max error %.8g\n",
                count, side, side, statistics.graphCaptures, statistics.graphLaunches,
                statistics.graphFallbacks, error);
    const size_t chartBytes = size_t(side) * side * 3 * sizeof(float) * 11;
    const size_t limit = std::min(size_t(lightmap::denoising::queuedChartLimit),
                                  lightmap::denoising::queueBudgetBytes / chartBytes);
    uint32_t expectedBatches = 0, expectedExecutions = 0;
    for (uint32_t chart = 0; chart < count; ++chart)
      if (!mixedMasks || chart % 7 != 0) {
        ++expectedBatches;
        expectedExecutions += mixedMasks && chart % 5 == 0 ? 1 : 2;
      }
    require(statistics.charts == count && statistics.batches == expectedBatches &&
                statistics.executions == expectedExecutions &&
                statistics.synchronizations == (count + limit - 1) / limit,
            "Chart queues flush at both count and host-byte bounds without dropping packets");
    if (statistics.graphLaunches && !statistics.graphFallbacks)
      require(statistics.graphLaunches == (count + limit - 1) / limit &&
                  statistics.batchBufferBytes == lightmap::denoising::queueBudgetBytes,
              "CUDA batches preserve constant-band masks and capture one graph per bounded queue");
    require(statistics.peakHostBytes == limit * chartBytes &&
                statistics.peakHostBytes <= lightmap::denoising::queueBudgetBytes,
            "Queued host image storage respects its bounded scratch target");
    for (uint32_t i = 0; i < values.size(); ++i) {
      require(values[i].statistics == original[i].statistics &&
                  values[i].irradiance[0][3] == original[i].irradiance[0][3],
              "Queued readbacks preserve every chart's metadata and direct-light channel");
      require(std::isfinite(values[i].irradiance[0][0]) && std::isfinite(values[i].irradiance[1][0]),
              "Every queued packet completes before reconstruction");
    }
    std::printf("OIDN queue %u x %ux%u: %u packets, %u waits, %llu host bytes\n", count, side, side,
                statistics.batches, statistics.synchronizations,
                static_cast<unsigned long long>(statistics.peakHostBytes));
  }
} // namespace
int main(int argc, char **argv) try {
#ifdef REMIX_OIDN
  if ((argc == 2 || argc == 3) && std::string_view(argv[1]) == "--cpu-benchmark") {
    uint32_t workers = 1;
    if (argc == 3) {
      const std::string_view text(argv[2]);
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), workers);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("Invalid CPU benchmark worker count");
    }
    remix::forward::test::denoiseBenchmark(workers);
    return 0;
  }
#endif
  queueFailures();
  {
    lightmap::Layout probeLayout;
    probeLayout.charts.resize(4096, {0, 0, 0, 16, 16});
    std::vector<std::vector<uint32_t>> points(4096, {0});
    const auto probes = lightmap::denoising::deviceProbePlan(probeLayout, points, 16);
    require(probes.size() == 1 && probes[0].population == 4096 && probes[0].charts[0].size() == 16,
            "Chart-heavy device probes retain population and bound sample count");
    for (const auto &group : probes)
      for (const auto &batch : group.charts)
        for (uint32_t chart : batch) require(chart < 4096, "Device probes preserve source chart identities");
    lightmap::Layout varied;
    std::vector<std::vector<uint32_t>> variedPoints(1800, {0});
    for (uint32_t side : {16u, 32u, 128u})
      for (uint32_t i = 0; i < 600; ++i) varied.charts.push_back({0, 0, 0, side, side});
    const auto variedProbes = lightmap::denoising::deviceProbePlan(varied, variedPoints, 16);
    uint64_t probeBytes = 0;
    for (const auto &group : variedProbes)
      for (const auto &batch : group.charts)
        probeBytes += uint64_t(group.width) * group.height * 3 * 9 * sizeof(float) * batch.size();
    require(variedProbes.size() == 3 && probeBytes <= (8u << 20),
            "Three chart-size bins share one bounded calibration input budget");
    auto strips = probeLayout;
    strips.charts.assign(600, {0, 0, 0, 256, 16});
    std::vector<std::vector<uint32_t>> stripPoints(600, {0});
    const auto stripProbes = lightmap::denoising::deviceProbePlan(strips, stripPoints, 16);
    require(stripProbes.size() == 1 && stripProbes[0].width == 256 &&
                stripProbes[0].height == 16 && stripProbes[0].population == 600,
            "Narrow charts receive measured device selection within the same padded pixel budget");
    strips.charts.assign(600, {0, 0, 0, 1024, 17});
    require(lightmap::denoising::deviceProbePlan(strips, stripPoints, 16).empty(),
            "Network alignment counts toward the maximum calibration chart work");
    auto tiny = probeLayout;
    tiny.charts.resize(600);
    std::vector<std::vector<uint32_t>> tinyPoints(600, {0});
    require(lightmap::denoising::deviceProbePlan(tiny, tinyPoints, 16).empty(),
            "Many tiny charts still skip calibration when total padded work is small");
    probeLayout.charts.resize(100);
    points.resize(100);
    require(lightmap::denoising::deviceProbePlan(probeLayout, points, 16).empty(),
            "Small workloads avoid device calibration overhead");
    probeLayout.charts.resize(600, {0, 0, 0, 512, 512});
    points.resize(600, {0});
    require(lightmap::denoising::deviceProbePlan(probeLayout, points, 16).empty(),
            "Large-image workloads retain the default device");
    require(lightmap::denoising::preferCpuQueues(10, 17, 25) &&
                !lightmap::denoising::preferCpuDevice(17, 25) &&
                !lightmap::denoising::preferCpuQueues(13, 17, 25) &&
                !lightmap::denoising::preferCpuQueues(10, 20, 15) &&
                !lightmap::denoising::preferCpuQueues(0, 17, 25) &&
                !lightmap::denoising::preferCpuQueues(10, std::numeric_limits<double>::infinity(), 25),
            "Parallel device selection compares measured CPU queues with both serial CPU and default");
    require(lightmap::denoising::preferCpuDevice(1, 2) &&
                !lightmap::denoising::preferCpuDevice(1.5, 2) &&
                !lightmap::denoising::preferCpuDevice(0, 2) &&
                !lightmap::denoising::preferCpuDevice(std::numeric_limits<double>::infinity(), 2),
            "CPU selection requires a substantial finite timing margin");
  }

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
#ifdef REMIX_OIDN
  deviceCalibrationPreservesInput();
#endif
  largeBatch();
  paddedCharts();
  queuedCharts(16, 40); // chart-count bound, then a partial tail
  queuedCharts(64, 18); // byte bound, then a partial tail
  queuedCharts(16, 40, true); // fully constant charts and independently changing band masks
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
#ifdef REMIX_OIDN
  parallelCharts(layout, raw);
#endif
  std::vector<std::array<uint32_t, 2>> progress;
  const auto statistics = lightmap::denoise(layout, values, {}, [&](uint32_t completed, uint32_t total) {
    progress.push_back({completed, total});
  });
  auto pageable = raw;
  const auto reference = lightmap::denoise(layout, pageable, {}, {}, lightmap::HostTransfers::Pageable, lightmap::FilterDispatch::Ordinary);
  float transferError = 0;
  for (size_t point = 0; point < values.size(); ++point)
    for (uint32_t band = 0; band < 9; ++band)
      for (uint32_t c = 0; c < 3; ++c)
        transferError = std::max(transferError,
            std::abs(values[point].irradiance[band][c] - pageable[point].irradiance[band][c]));
  require(transferError < .0001f, "Graph and ordinary submission preserve the same isolated chart fields");
  require(statistics.stagingBufferBytes <= lightmap::denoising::queueBudgetBytes &&
              reference.stagingBufferBytes == 0,
          "Pinned staging remains bounded and pageable mode allocates none");
  require(statistics.deviceType != 1 || statistics.stagingBufferBytes == 0,
          "CPU devices retain direct transfers without allocating pinned staging");
  std::printf("Transfer parity: maxerror %.8g, pinned %.6fs, pageable %.6fs, staging %llu bytes\n",
              transferError, statistics.seconds, reference.seconds,
              static_cast<unsigned long long>(statistics.stagingBufferBytes));
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
  require(statistics.batches == 3 && statistics.synchronizations == 2,
          "Equal-sized charts queue separate packets with one synchronization per bounded shape group");
  require(statistics.monitoredExecutions == (statistics.deviceType == 1 ? 12u : 0u),
          "Small GPU fields avoid internal progress callbacks; CPU filters retain in-flight cancellation");
  require(statistics.peakHostBytes == 3u * 64 * 64 * 3 * 11 * sizeof(float) &&
              statistics.imageBufferBytes == 64u * 64 * 3 * 9 * sizeof(float),
          "Pending chart arrays are accounted separately from the one reusable device image buffer");
  std::printf("OIDN %u device type %d: chart isolation, signed SH, metadata and cancellation passed; "
              "L0 RMSE %.5f -> %.5f, directional RMSE %.5f -> %.5f, %.3f s\n",
              lightmap::denoiserVersion(), statistics.deviceType, std::sqrt(before / 8719),
              std::sqrt(after / 8719), std::sqrt(directionBefore / 8719), std::sqrt(directionAfter / 8719),
              statistics.seconds);
} catch (const std::exception &error) {
  std::fprintf(stderr, "Lightmap denoising test failed: %s\n", error.what());
  return 1;
}
