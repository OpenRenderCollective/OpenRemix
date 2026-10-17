#include "forward/lightmap/denoise.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
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
      OIDNDevice device = nullptr;
      OIDNFilter filter = nullptr;
      OIDNBuffer buffer = nullptr;
      size_t capacity = 0;
      ~Filter() {
        if (filter)
          oidnReleaseFilter(filter);
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
        device = oidnNewDevice(OIDN_DEVICE_TYPE_DEFAULT);
        check();
        if (!device)
          throw std::runtime_error("No OIDN lightmap device");
        oidnCommitDevice(device);
        check();
        filter = oidnNewFilter(device, "RTLightmap");
        check();
        oidnSetFilterInt(filter, "quality", OIDN_QUALITY_HIGH);
        oidnSetFilterInt(filter, "maxMemoryMB", 256);
        // Fixed across charts, avoiding per-chart auto-exposure differences.
        oidnSetFilterFloat(filter, "inputScale", 1);
        oidnSetFilterProgressMonitorFunction(
            filter,
            [](void *user, double) { return !static_cast<std::stop_token *>(user)->stop_requested(); },
            &cancelled);
        check();
      }
      void run(std::vector<float> &image, uint32_t width, uint32_t height, bool directional) {
        // A trained filter can introduce a small bias even into a constant field.
        // Preserve those fields exactly and avoid a device/model dispatch for them.
        bool constant = true;
        for (size_t i = 3; i < image.size() && constant; ++i)
          constant = std::abs(image[i] - image[i % 3]) <= 1e-7f;
        if (constant)
          return;
        const auto bytes = image.size() * sizeof(float);
        if (bytes > capacity) {
          if (buffer)
            oidnReleaseBuffer(buffer);
          buffer = oidnNewBufferWithStorage(device, bytes, OIDN_STORAGE_DEVICE);
          capacity = bytes;
          check();
        }
        oidnWriteBuffer(buffer, 0, bytes, image.data());
        oidnSetFilterImage(filter, "color", buffer, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
        oidnSetFilterImage(filter, "output", buffer, OIDN_FORMAT_FLOAT3, width, height, 0, 0, 0);
        oidnSetFilterBool(filter, "directional", directional);
        oidnCommitFilter(filter);
        check();
        oidnExecuteFilter(filter);
        check();
        oidnReadBuffer(buffer, 0, bytes, image.data());
        check();
        for (const auto value : image)
          if (!std::isfinite(value))
            throw std::runtime_error("OIDN returned nonfinite lightmap data");
      }
    };
  } // namespace
#endif
  DenoiseStatistics denoise(const Layout &layout, std::span<bake::Result> results,
                            std::stop_token cancelled) {
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
    Filter filter;
    filter.initialize(cancelled);
    statistics.deviceType = oidnGetDeviceInt(filter.device, "type");
    filter.check();
    std::vector<std::vector<uint32_t>> chartPoints(layout.charts.size());
    for (uint32_t i = 0; i < results.size(); ++i) {
      if (layout.pointCharts[i] >= chartPoints.size())
        throw std::invalid_argument("Invalid denoising chart");
      chartPoints[layout.pointCharts[i]].push_back(i);
    }
    for (uint32_t chart = 0; chart < layout.charts.size(); ++chart) {
      checkCancellation(cancelled);
      if (chartPoints[chart].empty())
        continue;
      const auto &a = layout.charts[chart];
      if (!a.width || !a.height || a.page >= layout.pages || uint64_t(a.x) + a.width > layout.pageSize ||
          uint64_t(a.y) + a.height > layout.pageSize)
        throw std::invalid_argument("Invalid denoising allocation");
      const size_t origin = (size_t(a.page) * layout.pageSize + a.y) * layout.pageSize + a.x;
      // Three RGB host images for the largest chart, never a copy of the whole atlas.
      std::vector<float> original(size_t(a.width) * a.height * 3), l0(original.size()), band(original.size());
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
    }
    statistics.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return statistics;
#endif
  }
} // namespace remix::forward::lightmap
