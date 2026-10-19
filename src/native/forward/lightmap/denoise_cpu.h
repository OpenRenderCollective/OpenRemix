#pragma once
#include "forward/lightmap/denoise_device.h"
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace remix::forward::lightmap::denoising {
#ifdef REMIX_OIDN
  // Prepared chart ownership and ordering are shared read-only. Each worker may
  // write only points owned by its assigned charts. Progress is serialized.
  void filterChartQueues(const Layout &layout, std::span<bake::Result> results,
                         std::span<const uint32_t> order,
                         const std::vector<std::vector<uint32_t>> &chartPoints, size_t largestImage,
                         DeviceChoice device, HostTransfers transfers, FilterDispatch dispatch,
                         std::stop_token cancelled, DenoiseStatistics &statistics,
                         const std::function<void(uint32_t, uint32_t)> &progress);
#endif
  // Every queue owns its filters and scratch. An error cancels peers, and every
  // queue joins before borrowed chart arrays or callback state can be released.
  template <typename Task> void cpuQueues(uint32_t count, std::stop_token cancelled, Task task) {
    if (!count || count > 4)
      throw std::invalid_argument("CPU chart queue count outside its budget");
    std::stop_source cancellation;
    std::stop_callback forward(cancelled, [&] { cancellation.request_stop(); });
    std::exception_ptr failure;
    std::mutex mutex;
    auto run = [&](uint32_t index) {
      try {
        task(index, cancellation.get_token());
      } catch (...) {
        {
          std::lock_guard lock(mutex);
          if (!failure)
            failure = std::current_exception();
        }
        cancellation.request_stop();
      }
    };
    std::vector<std::jthread> threads;
    threads.reserve(count - 1);
    try {
      for (uint32_t i = 1; i < count; ++i)
        threads.emplace_back(run, i);
      run(0);
    } catch (...) {
      cancellation.request_stop();
      throw;
    }
    threads.clear();
    if (failure)
      std::rethrow_exception(failure);
  }
} // namespace remix::forward::lightmap::denoising
