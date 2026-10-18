#include "forward/lightmap/denoise.h"
#include "forward/lightmap/denoise_chart.h"
#include "forward/lightmap/denoise_filter.h"
#include "forward/lightmap/denoise_device.h"
#include <algorithm>
#include <chrono>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <utility>
#ifdef REMIX_OIDN
#include <OpenImageDenoise/config.h>
static_assert(OIDN_VERSION >= 20500, "Lightmap denoising requires Open Image Denoise 2.5 or newer");
#endif

namespace remix::forward::lightmap {
  uint32_t denoiserVersion() {
#ifdef REMIX_OIDN
    return OIDN_VERSION;
#else
    return 0;
#endif
  }
#ifdef REMIX_OIDN
  namespace {
    // Match OIDN 2.5's 16-pixel U-Net input padding, allowing neighbouring chart
    // sizes to share committed shapes. Unknown versions retain exact chart sizes.
    constexpr uint32_t imageAlignment = OIDN_VERSION >= 20500 && OIDN_VERSION < 20600 ? 16 : 1;
    auto imageShape(const Allocation &chart) {
      return std::pair{denoising::alignedExtent(chart.width, imageAlignment),
                        denoising::alignedExtent(chart.height, imageAlignment)};
    }
    using denoising::checkCancellation;
    using denoising::Duration;
    using denoising::Packet;
    using denoising::Filter;
  } // namespace
#endif
  DenoiseStatistics denoise(const Layout &layout, std::span<bake::Result> results, std::stop_token cancelled,
                            std::function<void(uint32_t, uint32_t)> progress, HostTransfers transfers, FilterDispatch dispatch) {
#ifndef REMIX_OIDN
    throw std::runtime_error("Lightmap denoising requires a build configured with oidn_root");
#else
    const auto start = std::chrono::steady_clock::now();
    if (results.size() != layout.points.size() || layout.pointCharts.size() != results.size() ||
        layout.addresses.size() != results.size() ||
        layout.owners.size() != uint64_t(layout.pageSize) * layout.pageSize * layout.pages)
      throw std::invalid_argument("Incomplete lightmap denoising layout");
    DenoiseStatistics statistics;
    if (results.empty())
      return statistics;
    checkCancellation(cancelled);
    std::vector<uint32_t> order(layout.charts.size());
    std::iota(order.begin(), order.end(), 0u);
    size_t largestImage = 0;
    for (const auto &a : layout.charts) {
      if (!a.width || !a.height || a.page >= layout.pages || uint64_t(a.x) + a.width > layout.pageSize ||
          uint64_t(a.y) + a.height > layout.pageSize)
        throw std::invalid_argument("Invalid denoising allocation");
      const auto [width, height] = imageShape(a);
      const size_t fieldBytes = size_t(width) * height * 3 * sizeof(float);
      largestImage = std::max(largestImage, fieldBytes * denoising::batchFields(fieldBytes));
    }
    // Chart-local donors make charts independent. Group equal image shapes so
    // the same committed network can serve all of them without re-preparation.
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const auto &first = layout.charts[a], &second = layout.charts[b];
      return imageShape(first) > imageShape(second);
    });
    std::vector<std::vector<uint32_t>> chartPoints(layout.charts.size());
    for (uint32_t i = 0; i < results.size(); ++i) {
      if (layout.pointCharts[i] >= chartPoints.size())
        throw std::invalid_argument("Invalid denoising chart");
      chartPoints[layout.pointCharts[i]].push_back(i);
    }
    const auto device = denoising::chooseDevice(layout, results, chartPoints, imageAlignment,
        largestImage, transfers, dispatch, cancelled, statistics);
    struct PendingChart {
      std::unique_ptr<denoising::Chart> image;
      std::span<float> fields;
      uint32_t contentWidth, contentHeight;
      bool enqueued;
    };
    // Declaration order matters: Filter retires copies before pending host
    // arrays are destroyed, including on cancellation or an exception.
    std::vector<PendingChart> pending;
    Filter filter(statistics, largestImage, transfers, dispatch, device);
    filter.initialize(cancelled);
    const auto total = uint32_t(std::count_if(chartPoints.begin(), chartPoints.end(),
                                              [](const auto &points) { return !points.empty(); }));
    if (progress)
      progress(0, total);
    uint64_t pendingBytes = 0;
    auto flush = [&] {
      std::vector<Packet> deferred;
      for (const auto &chart : pending)
        if (!chart.enqueued) deferred.push_back({chart.fields, chart.contentWidth, chart.contentHeight});
      if (!deferred.empty()) {
        const auto width = pending.front().image->width(), height = pending.front().image->height();
        if (!filter.enqueueCharts(deferred, width, height))
          for (const auto &packet : deferred)
            filter.enqueue(packet.fields, width, height, 0, packet.contentWidth, packet.contentHeight);
      }
      filter.finish();
      for (auto &chart : pending) {
        const Duration duration(statistics.chartSeconds);
        filter.validate(chart.fields);
        chart.image->apply(0, denoising::coefficientCount, chart.fields);
        chart.image->finish();
        ++statistics.charts;
        if (progress)
          progress(statistics.charts, total);
      }
      pending.clear();
      pendingBytes = 0;
    };
    uint32_t pendingWidth = 0, pendingHeight = 0;
    for (const auto chart : order) {
      checkCancellation(cancelled);
      if (chartPoints[chart].empty())
        continue;
      const auto &a = layout.charts[chart];
      const auto [width, height] = imageShape(a);
      const auto fieldBytes = uint64_t(width) * height * 3 * sizeof(float);
      const auto batch = denoising::batchFields(size_t(fieldBytes));
      const auto chartBytes = fieldBytes * (2 + batch);
      if (!pending.empty() && (width != pendingWidth || height != pendingHeight ||
                               pendingBytes + chartBytes > denoising::queueBudgetBytes ||
                               pending.size() >= denoising::queuedChartLimit))
        flush();
      std::unique_ptr<denoising::Chart> image;
      {
        const Duration duration(statistics.chartSeconds);
        image = std::make_unique<denoising::Chart>(layout, chart, results, chartPoints[chart], cancelled,
                                                  imageAlignment);
      }
      statistics.texels += uint64_t(a.width) * a.height;
      statistics.peakHostBytes = std::max(statistics.peakHostBytes, pendingBytes + image->hostBytes());
      if (batch == denoising::coefficientCount) {
        pendingWidth = width;
        pendingHeight = height;
        std::span<float> fields;
        {
          const Duration duration(statistics.chartSeconds);
          fields = image->pack(0, batch);
        }
        pendingBytes += image->hostBytes();
        const bool defer = filter.canBatch(width, height);
        pending.push_back({std::move(image), fields, a.width, a.height, !defer}); // retain arrays before any async copy
        if (!defer) filter.enqueue(fields, width, height, 0, a.width, a.height);
        continue;
      }
      flush();
      for (uint32_t first = 0; first < denoising::coefficientCount; first += batch) {
        checkCancellation(cancelled);
        const auto count = std::min(batch, denoising::coefficientCount - first);
        std::span<float> fields;
        {
          const Duration duration(statistics.chartSeconds);
          fields = image->pack(first, count);
        }
        filter.run(fields, width, height, first, a.width, a.height);
        {
          const Duration duration(statistics.chartSeconds);
          image->apply(first, count, fields);
        }
      }
      {
        const Duration duration(statistics.chartSeconds);
        image->finish();
      }
      ++statistics.charts;
      if (progress)
        progress(statistics.charts, total);
    }
    flush();
    statistics.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return statistics;
#endif
  }
} // namespace remix::forward::lightmap
