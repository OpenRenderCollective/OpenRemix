#include "platform/gpu_timings.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
  remix::gpu::PassRange range(const char *name, uint32_t begin, uint32_t end, uint32_t parent = remix::gpu::noPass,
                              uint32_t depth = 0) {
    remix::gpu::PassRange r;
    std::snprintf(r.name, sizeof(r.name), "%s", name);
    r.width = 1920;
    r.height = 1080;
    r.begin = begin;
    r.end = end;
    r.parent = parent;
    r.depth = depth;
    return r;
  }
} // namespace

int main() {
  using namespace remix::gpu;
  // Query 0 and 9 bracket the frame. "Tracer" (1..6) contains "Trace" (2..3)
  // and "Denoise" (4..5); "UI" (7..8) is top-level. One tick is 1 ms (1e6 ns).
  const uint64_t stamps[] = {100, 101, 102, 112, 113, 116, 118, 118, 119, 120};
  const std::vector<PassRange> ranges = {range("Tracer", 1, 6), range("Trace", 2, 3, 0, 1),
                                         range("Denoise", 4, 5, 0, 1), range("UI", 7, 8)};
  auto t = resolve(ranges, stamps, 0, 9, 64, 1e6);
  require(t.available, "Timings unavailable");
  require(near(t.frameMilliseconds, 20), "Frame span");
  require(near(t.passes[0].milliseconds, 17) && near(t.passes[0].selfMilliseconds, 4), "Container self time");
  require(near(t.passes[1].milliseconds, 10) && near(t.passes[1].selfMilliseconds, 10), "Nested pass");
  require(near(t.passes[2].milliseconds, 3), "Second nested pass");
  require(near(t.passes[3].milliseconds, 1), "Top-level pass");
  require(near(t.passMilliseconds, 18), "Top-level passes");
  require(near(t.untimedMilliseconds, 2), "Untimed");
  double self = 0;
  for (const auto &pass : t.passes)
    self += pass.selfMilliseconds;
  require(near(self + t.untimedMilliseconds, t.frameMilliseconds), "Own times and untimed add up to the frame");
  const auto order = byCost(t);
  require(order.size() == 4 && order[0] == 1 && order[1] == 0 && order[2] == 2 && order[3] == 3, "Order by own time");
  require(std::strcmp(t.passes[1].name, "Trace") == 0 && t.passes[1].width == 1920, "Name and size kept");

  // A 32-bit timestamp counter wrapping inside the frame.
  const uint64_t wrapped[] = {0xfffffffeull, 0x1ull, 0x3ull};
  t = resolve({range("Wrap", 0, 1)}, wrapped, 0, 2, 32, 1e6);
  require(near(t.passes[0].milliseconds, 3) && near(t.frameMilliseconds, 5), "Counter wrap");

  // An open range (never closed) counts zero.
  auto open = range("Open", 1, 1);
  t = resolve({open}, stamps, 0, 9, 64, 1e6);
  require(near(t.passes[0].milliseconds, 0) && near(t.untimedMilliseconds, 20), "Open range");
  std::puts("GPU pass timings: ok");
  return 0;
}
