#include "forward/lightmap/denoise.h"
#include "forward/lightmap/denoise_chart.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>
#include <tuple>
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
      Filter(DenoiseStatistics &statistics, size_t requiredCapacity)
          : requiredCapacity(requiredCapacity), statistics(statistics) {}
      ~Filter() {
        // Even cancellation/errors must retire queued work before its inputs,
        // callbacks, filters or buffers can be released.
        if (inFlight)
          oidnSyncDevice(device);
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
      void run(std::span<float> images, uint32_t width, uint32_t height, uint32_t first) {
        checkCancellation(cancellation);
        const size_t plane = size_t(width) * height * 3;
        const uint32_t count = uint32_t(images.size() / plane);
        std::array<bool, denoising::coefficientCount> active{};
        bool any = false;
        for (uint32_t slot = 0; slot < count; ++slot) {
          const auto image = images.subspan(size_t(slot) * plane, plane);
          // Preserve constant fields exactly, avoiding both neural bias and work.
          bool constant = true;
          for (size_t i = 3; i < image.size() && constant; ++i)
            constant = std::abs(image[i] - image[i % 3]) <= 1e-7f;
          active[slot] = !constant;
          statistics.constantFields += uint32_t(constant);
          any |= !constant;
        }
        if (!any)
          return;
        const auto bytes = images.size() * sizeof(float);
        if (bytes > capacity) {
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
        oidnWriteBuffer(buffer, 0, bytes, images.data());
        check();
        for (uint32_t slot = 0; slot < count; ++slot) {
          if (!active[slot])
            continue;
          checkCancellation(cancellation);
          inFlight = true;
          oidnExecuteFilterAsync(models[first + slot].handle);
          check();
          ++statistics.executions;
          statistics.monitoredExecutions += uint32_t(models[first + slot].monitored);
        }
        // One ordered-device fence per chart batch. No output or model is
        // touched until OIDN reports all queued operations complete.
        oidnSyncDevice(device);
        inFlight = false;
        check();
        ++statistics.batches;
        oidnReadBuffer(buffer, 0, bytes, images.data());
        check();
        statistics.transferBytes += 2 * bytes;
        for (const auto value : images)
          if (!std::isfinite(value))
            throw std::runtime_error("OIDN returned nonfinite lightmap data");
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
      const size_t fieldBytes = size_t(a.width) * a.height * 3 * sizeof(float);
      largestImage = std::max(largestImage, fieldBytes * denoising::batchFields(fieldBytes));
    }
    // Chart-local donors make charts independent. Group equal image shapes so
    // the same committed network can serve all of them without re-preparation.
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const auto &first = layout.charts[a], &second = layout.charts[b];
      return std::tie(first.width, first.height) > std::tie(second.width, second.height);
    });
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
    for (const auto chart : order) {
      checkCancellation(cancelled);
      if (chartPoints[chart].empty())
        continue;
      const auto &a = layout.charts[chart];
      denoising::Chart image(layout, chart, results, chartPoints[chart], cancelled);
      statistics.peakHostBytes = std::max(statistics.peakHostBytes, image.hostBytes());
      const auto batch = image.fieldsPerBatch();
      for (uint32_t first = 0; first < denoising::coefficientCount; first += batch) {
        checkCancellation(cancelled);
        const auto count = std::min(batch, denoising::coefficientCount - first);
        auto fields = image.pack(first, count);
        filter.run(fields, a.width, a.height, first);
        image.apply(first, count, fields);
      }
      image.finish();
      ++statistics.charts;
      statistics.texels += uint64_t(a.width) * a.height;
      if (progress)
        progress(statistics.charts, total);
    }
    statistics.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return statistics;
#endif
  }
} // namespace remix::forward::lightmap
