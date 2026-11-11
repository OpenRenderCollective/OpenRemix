#include "platform/frame_latency.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

namespace remix::latency {
#if defined(_WIN32)
  namespace {
    int64_t frequency() {
      static const int64_t value = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? int64_t(f.QuadPart) : int64_t(1);
      }();
      return value;
    }
  } // namespace
  int64_t hostNanosecondsFromDomain(uint64_t raw) {
    // Whole seconds and the remainder separately: ticks * 1e9 would overflow.
    const int64_t f = frequency(), ticks = int64_t(raw);
    return ticks / f * 1000000000 + ticks % f * 1000000000 / f;
  }
  int64_t hostNanoseconds() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return hostNanosecondsFromDomain(uint64_t(counter.QuadPart));
  }
#else
  int64_t hostNanosecondsFromDomain(uint64_t raw) { return int64_t(raw); }
  int64_t hostNanoseconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return int64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
  }
#endif
} // namespace remix::latency
