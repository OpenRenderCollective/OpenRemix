#pragma once
#include "forward/lightmap/denoise_filter.h"
#include <array>

namespace remix::forward::lightmap::denoising {
  struct DeviceProbe {
    uint32_t width, height;
    uint32_t population; // usable charts in this size bin, including other aspect ratios
    std::array<std::vector<uint32_t>, 2> charts;
  };
  // Representative small-chart groups, with at most eight MiB of packed input.
  // Large-image workloads and small maps keep OIDN's ordinary device choice.
  std::vector<DeviceProbe> deviceProbePlan(const Layout &layout,
      std::span<const std::vector<uint32_t>> chartPoints, uint32_t alignment);
  bool preferCpuDevice(double cpu, double ordinary);
#ifdef REMIX_OIDN
  FilterDevice chooseDevice(const Layout &layout, std::span<bake::Result> results,
      std::span<const std::vector<uint32_t>> chartPoints, uint32_t alignment,
      size_t requiredCapacity, HostTransfers transfers, FilterDispatch dispatch,
      std::stop_token cancelled, DenoiseStatistics &statistics);
#endif
}
