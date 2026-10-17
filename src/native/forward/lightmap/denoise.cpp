#include "forward/lightmap/denoise.h"
#include "forward/lightmap/denoise_chart.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef REMIX_OIDN
#include <OpenImageDenoise/oidn.h>
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
    void checkCancellation(std::stop_token cancelled) {
      if (cancelled.stop_requested())
        throw std::runtime_error("Lightmap denoising superseded");
    }
    struct Filter {
      struct Model {
        OIDNFilter handle = nullptr;
        uint32_t width = 0, height = 0;
        OIDNBuffer image = nullptr;
        size_t offset = 0;
        bool monitored = false;
      };
      OIDNDevice device = nullptr;
      std::array<Model, denoising::coefficientCount> models{}; // one immutable image binding per SH band
      OIDNBuffer buffer = nullptr;
      size_t capacity = 0;
      size_t requiredCapacity = 0;
      DenoiseStatistics &statistics;
      std::stop_token cancellation;
      bool inFlight = false;
      void retire() noexcept {
        if (inFlight) {
          oidnSyncDevice(device);
          inFlight = false;
          ++statistics.synchronizations;
        }
      }
      void finish() {
        retire();
        check();
      }
      Filter(DenoiseStatistics &statistics, size_t requiredCapacity)
          : requiredCapacity(requiredCapacity), statistics(statistics) {}
      ~Filter() {
        // Even cancellation/errors must retire queued work before its inputs,
        // callbacks, filters or buffers can be released.
        retire();
        for (const auto &model : models)
          if (model.handle)
            oidnReleaseFilter(model.handle);
        if (buffer)
          oidnReleaseBuffer(buffer);
        if (device)
          oidnReleaseDevice(device);
      }
      void check() {
        const char *message = nullptr;
        if (oidnGetDeviceError(device, &message) != OIDN_ERROR_NONE)
          throw std::runtime_error(std::string("OIDN lightmap: ") + (message ? message : "unknown error"));
      }
      void initialize(std::stop_token &cancelled) {
        cancellation = cancelled;
        device = oidnNewDevice(OIDN_DEVICE_TYPE_DEFAULT);
        check();
        if (!device)
          throw std::runtime_error("No OIDN lightmap device");
        oidnCommitDevice(device);
        check();
        for (uint32_t i = 0; i < models.size(); ++i) {
          auto &model = models[i];
          model.handle = oidnNewFilter(device, "RTLightmap");
          check();
          oidnSetFilterInt(model.handle, "quality", OIDN_QUALITY_HIGH);
          oidnSetFilterInt(model.handle, "maxMemoryMB", 256);
          // Fixed across charts, avoiding per-chart auto-exposure differences.
          oidnSetFilterFloat(model.handle, "inputScale", 1);
          oidnSetFilterBool(model.handle, "directional", i != 0);
          check();
        }
      }
      void enqueue(std::span<float> images, uint32_t width, uint32_t height, uint32_t first,
                    uint32_t contentWidth, uint32_t contentHeight) {
        checkCancellation(cancellation);
        const size_t plane = size_t(width) * height * 3;
        const uint32_t count = uint32_t(images.size() / plane);
        std::array<bool, denoising::coefficientCount> active{};
        bool any = false;
        for (uint32_t slot = 0; slot < count; ++slot) {
          const auto image = images.subspan(size_t(slot) * plane, plane);
          // Preserve constant fields exactly, avoiding both neural bias and work.
          bool constant = true;
          // Ignore synthetic network padding when deciding whether real lighting
          // is constant. Such fields remain exact and never enter the network.
          for (uint32_t y = 0; y < contentHeight && constant; ++y)
            for (uint32_t x = 0; x < contentWidth && constant; ++x)
              for (uint32_t c = 0; c < 3 && constant; ++c)
                constant = std::abs(image[(size_t(y) * width + x) * 3 + c] - image[c]) <= 1e-7f;
          active[slot] = !constant;
          statistics.constantFields += uint32_t(constant);
          any |= !constant;
        }
        if (!any)
          return;
        const auto bytes = images.size() * sizeof(float);
        if (bytes > capacity) {
          finish();
          if (buffer)
            oidnReleaseBuffer(buffer);
          capacity = std::max(bytes, requiredCapacity);
          buffer = oidnNewBufferWithStorage(device, capacity, OIDN_STORAGE_DEVICE);
          check();
          statistics.imageBufferBytes = capacity;
          // A new allocation may reuse a released handle's value.
          for (auto &model : models)
            model.image = nullptr;
        }
        // Commit every binding before queuing anything. Recommitting while an
        // earlier field is running would introduce another implicit device wait.
        for (uint32_t slot = 0; slot < count; ++slot) {
          if (!active[slot])
            continue;
          checkCancellation(cancellation);
          auto &model = models[first + slot];
          const size_t offset = size_t(slot) * plane * sizeof(float);
          if (model.image == buffer && model.width == width && model.height == height &&
              model.offset == offset)
            continue;
          finish(); // retire prior uses before changing a committed binding
          // OIDN warns that progress callbacks add significant GPU synchronization
          // overhead. Small fields already have bounded cancellation between bands.
          model.monitored =
              statistics.deviceType == OIDN_DEVICE_TYPE_CPU || uint64_t(width) * height >= (1u << 20);
          OIDNProgressMonitorFunction monitor = nullptr;
          if (model.monitored)
            monitor = [](void *user, double) {
              return !static_cast<std::stop_token *>(user)->stop_requested();
            };
          oidnSetFilterProgressMonitorFunction(model.handle, monitor,
                                               model.monitored ? &cancellation : nullptr);
          oidnSetFilterImage(model.handle, "color", buffer, OIDN_FORMAT_FLOAT3, width, height, offset, 0, 0);
          oidnSetFilterImage(model.handle, "output", buffer, OIDN_FORMAT_FLOAT3, width, height, offset, 0, 0);
          oidnCommitFilter(model.handle);
          check();
          model.image = buffer;
          model.width = width;
          model.height = height;
          model.offset = offset;
          ++statistics.preparations;
        }
        inFlight = true;
        oidnWriteBufferAsync(buffer, 0, bytes, images.data());
        check();
        for (uint32_t slot = 0; slot < count; ++slot) {
          if (!active[slot])
            continue;
          checkCancellation(cancellation);
          oidnExecuteFilterAsync(models[first + slot].handle);
          check();
          ++statistics.executions;
          statistics.monitoredExecutions += uint32_t(models[first + slot].monitored);
        }
        ++statistics.batches;
        oidnReadBufferAsync(buffer, 0, bytes, images.data());
        check();
        statistics.transferBytes += 2 * bytes;
      }
      void validate(std::span<const float> images) {
        for (const auto value : images)
          if (!std::isfinite(value))
            throw std::runtime_error("OIDN returned nonfinite lightmap data");
      }
      void run(std::span<float> images, uint32_t width, uint32_t height, uint32_t first,
                uint32_t contentWidth, uint32_t contentHeight) {
        try {
          enqueue(images, width, height, first, contentWidth, contentHeight);
          finish();
          validate(images);
        } catch (...) {
          retire(); // caller-owned host arrays must outlive even failed async copies
          throw;
        }
      }
    };
  } // namespace
#endif
  DenoiseStatistics denoise(const Layout &layout, std::span<bake::Result> results, std::stop_token cancelled,
                            std::function<void(uint32_t, uint32_t)> progress) {
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
    struct PendingChart {
      std::unique_ptr<denoising::Chart> image;
      std::span<float> fields;
    };
    // Declaration order matters: Filter retires copies before pending host
    // arrays are destroyed, including on cancellation or an exception.
    std::vector<PendingChart> pending;
    Filter filter(statistics, largestImage);
    filter.initialize(cancelled);
    statistics.deviceType = oidnGetDeviceInt(filter.device, "type");
    filter.check();
    std::vector<std::vector<uint32_t>> chartPoints(layout.charts.size());
    for (uint32_t i = 0; i < results.size(); ++i) {
      if (layout.pointCharts[i] >= chartPoints.size())
        throw std::invalid_argument("Invalid denoising chart");
      chartPoints[layout.pointCharts[i]].push_back(i);
    }
    const auto total = uint32_t(std::count_if(chartPoints.begin(), chartPoints.end(),
                                              [](const auto &points) { return !points.empty(); }));
    if (progress)
      progress(0, total);
    uint64_t pendingBytes = 0;
    auto flush = [&] {
      filter.finish();
      for (auto &chart : pending) {
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
      auto image = std::make_unique<denoising::Chart>(layout, chart, results, chartPoints[chart], cancelled,
                                                    imageAlignment);
      statistics.texels += uint64_t(a.width) * a.height;
      statistics.peakHostBytes = std::max(statistics.peakHostBytes, pendingBytes + image->hostBytes());
      if (batch == denoising::coefficientCount) {
        pendingWidth = width;
        pendingHeight = height;
        auto fields = image->pack(0, batch);
        pendingBytes += image->hostBytes();
        pending.push_back({std::move(image), fields}); // retain arrays before any async copy
        filter.enqueue(fields, width, height, 0, a.width, a.height);
        continue;
      }
      flush();
      for (uint32_t first = 0; first < denoising::coefficientCount; first += batch) {
        checkCancellation(cancelled);
        const auto count = std::min(batch, denoising::coefficientCount - first);
        auto fields = image->pack(first, count);
        filter.run(fields, width, height, first, a.width, a.height);
        image->apply(first, count, fields);
      }
      image->finish();
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
