#include "forward/lightmap/denoise.h"
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
        bool monitored = false;
      };
      OIDNDevice device = nullptr;
      std::array<Model, 2> models{}; // HDR and signed directional weights stay committed independently
      OIDNBuffer buffer = nullptr;
      size_t capacity = 0;
      size_t requiredCapacity = 0;
      DenoiseStatistics &statistics;
      std::stop_token cancellation;
      Filter(DenoiseStatistics &statistics, size_t requiredCapacity)
          : requiredCapacity(requiredCapacity), statistics(statistics) {}
      ~Filter() {
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
          oidnSetFilterBool(model.handle, "directional", i == 1);
          check();
        }
      }
      void run(std::vector<float> &image, uint32_t width, uint32_t height, bool directional) {
        checkCancellation(cancellation);
        // A trained filter can introduce a small bias even into a constant field.
        // Preserve those fields exactly and avoid a device/model dispatch for them.
        bool constant = true;
        for (size_t i = 3; i < image.size() && constant; ++i)
          constant = std::abs(image[i] - image[i % 3]) <= 1e-7f;
        if (constant) {
          ++statistics.constantFields;
          return;
        }
        const auto bytes = image.size() * sizeof(float);
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
        oidnWriteBuffer(buffer, 0, bytes, image.data());
        auto &model = models[directional ? 1 : 0];
        if (model.image != buffer || model.width != width || model.height != height) {
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
          oidnSetFilterImage(model.handle, "color", buffer, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
          oidnSetFilterImage(model.handle, "output", buffer, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
          oidnCommitFilter(model.handle);
          check();
          model.image = buffer;
          model.width = width;
          model.height = height;
          ++statistics.preparations;
        }
        check();
        oidnExecuteFilter(model.handle);
        check();
        oidnReadBuffer(buffer, 0, bytes, image.data());
        check();
        ++statistics.executions;
        statistics.monitoredExecutions += uint32_t(model.monitored);
        statistics.transferBytes += 2 * bytes;
        for (const auto value : image)
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
      largestImage = std::max(largestImage, size_t(a.width) * a.height * 3 * sizeof(float));
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
      const size_t origin = (size_t(a.page) * layout.pageSize + a.y) * layout.pageSize + a.x;
      // Three RGB host images for the largest chart, never a copy of the whole atlas.
      std::vector<float> original(size_t(a.width) * a.height * 3), l0(original.size()), band(original.size());
      statistics.peakHostBytes =
          std::max(statistics.peakHostBytes, uint64_t(original.size()) * sizeof(float) * 3);
      for (uint32_t y = 0; y < a.height; ++y)
        for (uint32_t x = 0; x < a.width; ++x) {
          const auto owner = layout.owners[origin + size_t(y) * layout.pageSize + x];
          if (owner >= results.size() || layout.pointCharts[owner] != chart)
            throw std::invalid_argument("Lightmap denoising requires chart-local dilation donors");
          for (uint32_t c = 0; c < 3; ++c) {
            const float value = results[owner].irradiance[0][c];
            if (!std::isfinite(value) || value < 0)
              throw std::invalid_argument("Invalid indirect L0");
            original[(size_t(y) * a.width + x) * 3 + c] = value;
          }
        }
      l0 = original;
      filter.run(l0, a.width, a.height, false);
      for (uint32_t coefficient = 1; coefficient < 9; ++coefficient) {
        checkCancellation(cancelled);
        for (uint32_t y = 0; y < a.height; ++y)
          for (uint32_t x = 0; x < a.width; ++x) {
            const auto owner = layout.owners[origin + size_t(y) * layout.pageSize + x];
            for (uint32_t c = 0; c < 3; ++c) {
              const size_t k = (size_t(y) * a.width + x) * 3 + c;
              // Cosine-convolved SH has |Ln/L0| <= 1.155 for L1 and .56 for L2.
              // Dividing by two keeps every normalized signed coefficient in [-1,1].
              const float value = results[owner].irradiance[coefficient][c];
              if (!std::isfinite(value))
                throw std::invalid_argument("Invalid indirect SH band");
              band[k] = original[k] > 1e-10f ? std::clamp(value / (2 * original[k]), -1.f, 1.f) : 0;
            }
          }
        filter.run(band, a.width, a.height, true);
        for (const auto point : chartPoints[chart]) {
          const size_t address = layout.addresses[point];
          if (address < origin)
            continue; // seam donors outside this chart rectangle remain raw
          const size_t offset = address - origin, x = offset % layout.pageSize, y = offset / layout.pageSize;
          if (x >= a.width || y >= a.height)
            continue;
          for (uint32_t c = 0; c < 3; ++c) {
            const size_t k = (y * a.width + x) * 3 + c;
            results[point].irradiance[coefficient][c] =
                std::clamp(band[k], -1.f, 1.f) * 2 * std::max(l0[k], 0.f);
          }
        }
      }
      for (const auto point : chartPoints[chart]) {
        const size_t address = layout.addresses[point];
        if (address < origin)
          continue;
        const size_t offset = address - origin, x = offset % layout.pageSize, y = offset / layout.pageSize;
        if (x >= a.width || y >= a.height)
          continue;
        for (uint32_t c = 0; c < 3; ++c)
          results[point].irradiance[0][c] = std::max(l0[(y * a.width + x) * 3 + c], 0.f);
      }
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
