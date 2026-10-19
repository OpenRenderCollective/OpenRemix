#pragma once
#include "forward/lightmap/denoise.h"
#include <chrono>
#include <memory>
#include <stdexcept>

#ifdef REMIX_OIDN
namespace remix::forward::lightmap::denoising {
  // CPU is a preference; unavailable creation retains ordinary OIDN fallback.
  enum class FilterDevice { Default, Cpu };
  inline void checkCancellation(std::stop_token cancelled) {
    if (cancelled.stop_requested())
      throw std::runtime_error("Lightmap denoising superseded");
  }
  class Duration {
  public:
    explicit Duration(double &seconds) : m_seconds(seconds) {}
    ~Duration() {
      m_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    }
    Duration(const Duration &) = delete;
    Duration &operator=(const Duration &) = delete;

  private:
    double &m_seconds;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
  };
  struct Packet {
    std::span<float> fields;
    uint32_t contentWidth, contentHeight;
  };
  // Backend-private OIDN/CUDA ownership. No driver or denoiser types escape
  // this interface; callers retain chart arrays until finish() retires copies.
  class Filter {
  public:
    Filter(DenoiseStatistics &statistics, size_t requiredCapacity, HostTransfers transfers,
           FilterDispatch dispatch, FilterDevice device = FilterDevice::Default, uint32_t cpuThreads = 0);
    ~Filter();
    Filter(const Filter &) = delete;
    Filter &operator=(const Filter &) = delete;
    void initialize(std::stop_token cancelled);
    void enqueue(std::span<float> images, uint32_t width, uint32_t height, uint32_t first,
                  uint32_t contentWidth, uint32_t contentHeight);
    bool canBatch(uint32_t width, uint32_t height) const;
    bool enqueueCharts(std::span<const Packet> packets, uint32_t width, uint32_t height);
    void finish();
    void validate(std::span<const float> images);
    void run(std::span<float> images, uint32_t width, uint32_t height, uint32_t first,
              uint32_t contentWidth, uint32_t contentHeight);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };
} // namespace remix::forward::lightmap::denoising
#endif
