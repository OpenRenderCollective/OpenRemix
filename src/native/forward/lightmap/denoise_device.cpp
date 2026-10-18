#include "forward/lightmap/denoise_device.h"
#include "forward/lightmap/denoise_chart.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#ifdef REMIX_OIDN
#include <OpenImageDenoise/oidn.h>
#include <cstdio>
#include <cstdlib>
#endif

namespace remix::forward::lightmap::denoising {
  namespace {
    constexpr uint32_t minimumCharts = 512, maximumExtent = 128, chartsPerProbe = 16;
    constexpr uint64_t minimumPixels = 1u << 20;
    uint32_t sizeBin(uint64_t pixels) { return pixels <= 512 ? 0 : pixels <= 2048 ? 1 : 2; }
  }
  std::vector<DeviceProbe> deviceProbePlan(const Layout &layout,
      std::span<const std::vector<uint32_t>> chartPoints, uint32_t alignment) {
    if (chartPoints.size() != layout.charts.size())
      throw std::invalid_argument("Device probe chart count mismatch");
    using Shape = std::pair<uint32_t, uint32_t>;
    std::array<std::map<Shape, std::vector<uint32_t>>, 3> bins;
    std::array<uint32_t, 3> populations{};
    uint64_t totalPixels = 0, smallPixels = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < layout.charts.size(); ++i) {
      if (chartPoints[i].empty()) continue;
      const auto &a = layout.charts[i];
      const uint32_t width = alignedExtent(a.width, alignment), height = alignedExtent(a.height, alignment);
      const uint64_t pixels = uint64_t(width) * height;
      if (!pixels) throw std::invalid_argument("Empty device probe chart");
      totalPixels += pixels;
      ++count;
      if (width > maximumExtent || height > maximumExtent) continue;
      smallPixels += pixels;
      const uint32_t bin = sizeBin(pixels);
      bins[bin][{width, height}].push_back(i);
      ++populations[bin];
    }
    if (count < minimumCharts || totalPixels < minimumPixels || double(smallPixels) < double(totalPixels) * .75) return {};
    std::vector<DeviceProbe> result;
    for (uint32_t bin = 0; bin < bins.size(); ++bin) {
      if (bins[bin].empty()) continue;
      const auto selected = std::max_element(bins[bin].begin(), bins[bin].end(), [](const auto &a, const auto &b) {
        return a.second.size() < b.second.size();
      });
      const auto [width, height] = selected->first;
      result.push_back({width, height, populations[bin], {}});
    }
    uint64_t bytes = 0;
    for (auto &probe : result) {
      const uint64_t fieldBytes = uint64_t(probe.width) * probe.height * 3 * sizeof(float) * coefficientCount;
      const uint64_t target = queueBudgetBytes / (2 * result.size());
      const uint32_t wanted = uint32_t(std::max<uint64_t>(1, std::min<uint64_t>(chartsPerProbe, target / fieldBytes)));
      const uint32_t bin = sizeBin(uint64_t(probe.width) * probe.height);
      const auto &charts = bins[bin].at({probe.width, probe.height});
      const uint32_t samples = std::min(wanted, uint32_t(charts.size()));
      for (uint32_t batch = 0; batch < 2; ++batch)
        for (uint32_t i = 0; i < samples; ++i)
          probe.charts[batch].push_back(charts[(uint64_t(i + batch * samples) * charts.size()) / (2 * samples)]);
      bytes += 2 * fieldBytes * samples;
    }
    if (bytes > queueBudgetBytes) return {};
    return result;
  }
  bool preferCpuDevice(double cpu, double ordinary) {
    // Sampling cannot perfectly predict every map shape or GPU scheduling gap.
    // Require a substantial measured margin before overriding the normal device.
    return std::isfinite(cpu) && std::isfinite(ordinary) && cpu > 0 && ordinary > 0 && cpu < ordinary * .65;
  }
#ifdef REMIX_OIDN
  namespace {
    struct ProbeBatch {
      uint32_t width, height;
      double population;
      std::vector<std::vector<float>> original;
      std::vector<Packet> packets;
    };
    double measureDevice(std::vector<ProbeBatch> &batches, FilterDevice device,
        size_t requiredCapacity, HostTransfers transfers, std::stop_token cancelled) {
      DenoiseStatistics scratch;
      // Host arrays belong to the caller and outlive the filter, including errors.
      Filter filter(scratch, requiredCapacity, transfers, FilterDispatch::Automatic, device);
      filter.initialize(cancelled);
      auto submit = [&](ProbeBatch &batch) {
        checkCancellation(cancelled);
        for (size_t i = 0; i < batch.packets.size(); ++i)
          std::copy(batch.original[i].begin(), batch.original[i].end(), batch.packets[i].fields.begin());
        if (!filter.enqueueCharts(batch.packets, batch.width, batch.height))
          for (const auto &packet : batch.packets)
            filter.enqueue(packet.fields, batch.width, batch.height, 0, packet.contentWidth, packet.contentHeight);
        filter.finish();
        for (const auto &packet : batch.packets) filter.validate(packet.fields);
      };
      submit(batches.front()); // initialize network/buffers before timing
      double projected = 0;
      for (auto &batch : batches) {
        const auto began = std::chrono::steady_clock::now();
        submit(batch);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        projected += seconds * batch.population / batch.packets.size();
      }
      const int expected = device == FilterDevice::Cpu ? OIDN_DEVICE_TYPE_CPU : OIDN_DEVICE_TYPE_CUDA;
      return scratch.deviceType == expected ? projected : std::numeric_limits<double>::infinity();
    }
  }
  FilterDevice chooseDevice(const Layout &layout, std::span<bake::Result> results,
      std::span<const std::vector<uint32_t>> chartPoints, uint32_t alignment,
      size_t requiredCapacity, HostTransfers transfers, FilterDispatch dispatch,
      std::stop_token cancelled, DenoiseStatistics &statistics) {
    if (dispatch != FilterDispatch::Automatic || std::getenv("OIDN_DEFAULT_DEVICE") ||
        oidnGetNumPhysicalDevices() < 1 || oidnGetPhysicalDeviceInt(0, "type") != OIDN_DEVICE_TYPE_CUDA)
      return FilterDevice::Default;
    const auto plan = deviceProbePlan(layout, chartPoints, alignment);
    if (plan.empty()) return FilterDevice::Default;
    const auto began = std::chrono::steady_clock::now();
    try {
      std::vector<ProbeBatch> batches;
      // Original packet data and writable outputs together stay within 16 MiB.
      std::vector<std::vector<float>> outputs;
      for (const auto &probe : plan)
        for (const auto &charts : probe.charts) {
          ProbeBatch batch{probe.width, probe.height, double(probe.population) / 2};
          for (uint32_t chart : charts) {
            checkCancellation(cancelled);
            Chart image(layout, chart, results, chartPoints[chart], cancelled, alignment);
            const auto fields = image.pack(0, coefficientCount);
            batch.original.emplace_back(fields.begin(), fields.end());
            outputs.emplace_back(fields.begin(), fields.end());
            const auto &a = layout.charts[chart];
            batch.packets.push_back({outputs.back(), a.width, a.height});
          }
          batches.push_back(std::move(batch));
        }
      statistics.cpuEstimateSeconds = measureDevice(batches, FilterDevice::Cpu, requiredCapacity, transfers, cancelled);
      statistics.defaultEstimateSeconds = measureDevice(batches, FilterDevice::Default, requiredCapacity, transfers, cancelled);
      statistics.deviceCalibrationSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
      statistics.calibrationCharts = 0;
      for (const auto &batch : batches) statistics.calibrationCharts += uint32_t(batch.packets.size());
      const bool cpu = preferCpuDevice(statistics.cpuEstimateSeconds, statistics.defaultEstimateSeconds);
      std::fprintf(stderr, "OIDN device calibration: %u chart probes, %.3f s; projected CPU %.3f s / default %.3f s; CPU %u\n",
          statistics.calibrationCharts, statistics.deviceCalibrationSeconds,
          statistics.cpuEstimateSeconds, statistics.defaultEstimateSeconds, unsigned(cpu));
      return cpu ? FilterDevice::Cpu : FilterDevice::Default;
    } catch (const std::exception &error) {
      statistics.deviceCalibrationSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
      checkCancellation(cancelled);
      oidnGetDeviceError(nullptr, nullptr); // optional calibration must not poison default creation
      std::fprintf(stderr, "OIDN device calibration unavailable: %s\n", error.what());
      return FilterDevice::Default;
    }
  }
#endif
}
