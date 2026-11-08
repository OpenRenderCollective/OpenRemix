#pragma once
#include <chrono>
#include <cstdint>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define REMIX_TIMING_TSC 1
#endif

namespace remix {
  struct ScopedMilliseconds {
    double &total;
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    ~ScopedMilliseconds() {
      total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
  };
  // A restartable ScopedMilliseconds.
  struct Stopwatch {
    double &total;
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    bool running = true;
    void stop() {
      if (running)
        total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
      running = false;
    }
    void start() {
      if (!running)
        begin = std::chrono::steady_clock::now();
      running = true;
    }
    ~Stopwatch() { stop(); }
  };
  template <class F> auto measured(double &total, F &&call) {
    ScopedMilliseconds time{total};
    return call();
  }

  // Low-overhead timestamps for timers nested in per-draw loops, where a
  // steady_clock read per draw would itself cost milliseconds per frame.
  // Accumulate raw ticks and convert once with tickMilliseconds().
  inline uint64_t ticks() {
#if defined(REMIX_TIMING_TSC)
    return __rdtsc();
#else
    return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
  }
  inline double tickMilliseconds(uint64_t count) {
#if defined(REMIX_TIMING_TSC)
    // Invariant TSC rate, calibrated once against the steady clock (2 ms).
    static const double perMillisecond = [] {
      using Clock = std::chrono::steady_clock;
      const auto start = Clock::now();
      const uint64_t first = __rdtsc();
      auto now = start;
      while (now - start < std::chrono::milliseconds(2))
        now = Clock::now();
      const uint64_t last = __rdtsc();
      const double elapsed = std::chrono::duration<double, std::milli>(now - start).count();
      return elapsed > 0 && last > first ? double(last - first) / elapsed : 1e6;
    }();
    return double(count) / perMillisecond;
#else
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::duration(count)).count();
#endif
  }
  struct ScopedTicks {
    uint64_t &total;
    uint64_t begin = ticks();
    ~ScopedTicks() { total += ticks() - begin; }
  };
} // namespace remix
