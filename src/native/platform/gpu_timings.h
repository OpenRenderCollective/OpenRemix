#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// GPU pass timings ("rtx.native.gpuPassTimers"): the Vulkan-free half. A frame
// is one command buffer on one queue; every timed pass is a range between two
// timestamps written at the bottom of the pipe (gpu_timers.h), so a pass's
// time is what the queue spent from the end of the preceding work to the end
// of the pass. Consecutive top-level passes therefore tile the frame; what no
// pass covers (barriers, small copies between passes) is reported as untimed.
namespace remix::gpu {
  inline constexpr uint32_t noPass = UINT32_MAX;
  // A range recorded into a frame's command buffer.
  struct PassRange {
    char name[56] = {};
    uint32_t width = 0, height = 0; // dispatch/render resolution; 0 when not an image pass
    uint32_t parent = noPass, depth = 0;
    uint32_t begin = 0, end = 0; // query indices; end == begin while the range is open
  };
  struct PassTiming {
    char name[56] = {};
    uint32_t width = 0, height = 0, depth = 0, parent = noPass;
    double milliseconds = 0;     // inclusive of nested passes
    double selfMilliseconds = 0; // exclusive of nested passes
  };
  struct FrameTimings {
    bool available = false;
    uint32_t truncated = 0; // passes dropped for lack of queries
    uint32_t queueFamily = 0, timestampValidBits = 0;
    uint64_t frame = 0;             // runtime frame number of the measured frame
    double frameMilliseconds = 0;   // first to last timestamp of the frame
    double passMilliseconds = 0;    // top-level passes
    double untimedMilliseconds = 0; // frame minus top-level passes
    double timestampPeriod = 0;     // nanoseconds per tick
    uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    std::vector<PassTiming> passes; // recording order
  };

  // Converts a frame's raw timestamps. stamps[i] is query i; frameBegin and
  // frameEnd bracket the whole command buffer. mask keeps timestampValidBits.
  inline FrameTimings resolve(const std::vector<PassRange> &ranges, const uint64_t *stamps, uint32_t frameBegin,
                              uint32_t frameEnd, uint32_t validBits, double period) {
    FrameTimings out;
    const uint64_t mask = validBits >= 64 ? UINT64_MAX : validBits ? (uint64_t(1) << validBits) - 1 : 0;
    auto elapsed = [&](uint32_t a, uint32_t b) { return double((stamps[b] - stamps[a]) & mask) * period / 1e6; };
    out.available = true;
    out.timestampValidBits = validBits;
    out.timestampPeriod = period;
    out.frameMilliseconds = elapsed(frameBegin, frameEnd);
    out.passes.resize(ranges.size());
    for (size_t i = 0; i < ranges.size(); ++i) {
      const auto &range = ranges[i];
      auto &pass = out.passes[i];
      std::memcpy(pass.name, range.name, sizeof(pass.name));
      pass.name[sizeof(pass.name) - 1] = 0;
      pass.width = range.width;
      pass.height = range.height;
      pass.depth = range.depth;
      pass.parent = range.parent;
      pass.milliseconds = range.end == range.begin ? 0 : elapsed(range.begin, range.end);
      pass.selfMilliseconds = pass.milliseconds;
    }
    for (const auto &pass : out.passes) {
      if (pass.parent == noPass)
        out.passMilliseconds += pass.milliseconds;
      else if (pass.parent < out.passes.size())
        out.passes[pass.parent].selfMilliseconds -= pass.milliseconds;
    }
    for (auto &pass : out.passes)
      pass.selfMilliseconds = std::max(0.0, pass.selfMilliseconds);
    out.untimedMilliseconds = std::max(0.0, out.frameMilliseconds - out.passMilliseconds);
    return out;
  }

  // Indices of passes by descending self time (ties keep recording order).
  inline std::vector<uint32_t> byCost(const FrameTimings &timings) {
    std::vector<uint32_t> order(timings.passes.size());
    for (uint32_t i = 0; i < order.size(); ++i)
      order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      return timings.passes[a].selfMilliseconds > timings.passes[b].selfMilliseconds;
    });
    return order;
  }
} // namespace remix::gpu
