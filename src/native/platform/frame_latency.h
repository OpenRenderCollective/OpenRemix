#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// Frame latency statistics (Diagnostics' Latency panel, timings.txt): the
// Vulkan-free half. Every frame the host starts (the frame number Reflex and
// Anti-Lag use) gets timestamps on one host clock (hostNanoseconds), at the
// points the low latency markers mark, from the caller's thread, the render
// thread and, converted from GPU timestamps, the GPU (frame_stamps.h).
// Like NVIDIA Reflex's own report, it measures from the frame's start (where
// the host reads its input, after the low latency hold) to the GPU finishing
// it; input sampling inside the host's frame is not seen.
namespace remix::latency {
  // Nanoseconds on the clock VK_KHR_calibrated_timestamps calls the host's
  // (QueryPerformanceCounter on Windows, CLOCK_MONOTONIC elsewhere).
  int64_t hostNanoseconds();
  // A raw host-domain value from vkGetCalibratedTimestampsKHR, in those units.
  int64_t hostNanosecondsFromDomain(uint64_t raw);

  enum Point : uint8_t {
    SleepStart,   // the low latency hold before the frame begins (Reflex sleep, Anti-Lag input stage)
    FrameStart,   // the host's frame begins: Present returned
    PresentCall,  // the host calls Present
    SubmitStart,  // recording begins (after the render thread's handoff, the fence, acquire)
    SubmitEnd,    // vkQueueSubmit returned
    PresentStart, // the frame goes to presentation
    PresentEnd,
    GpuStart, // the frame's command buffer, top and bottom of the pipe
    GpuEnd,
    PointCount
  };
  enum Stage : uint8_t {
    Hold,       // SleepStart → FrameStart
    Simulation, // FrameStart → PresentCall: the host's own frame
    Handoff,    // PresentCall → SubmitStart: waits for the previous frame, preparation, acquire
    Submit,     // SubmitStart → SubmitEnd: recording and submission
    Queued,     // SubmitEnd → GpuStart: submitted, waiting for the GPU
    Gpu,        // GpuStart → GpuEnd
    Present,    // PresentStart → PresentEnd
    StageCount
  };
  inline const char *stageName(Stage stage) {
    static const char *const names[] = {"Low latency hold",  "Game (host frame)",  "Handoff and preparation",
                                        "Record and submit", "Queued for the GPU", "GPU render",
                                        "Present call"};
    return stage < StageCount ? names[stage] : "";
  }
  inline constexpr uint32_t historyFrames = 64;

  struct FrameRecord {
    uint64_t frame = 0;
    std::array<int64_t, PointCount> at{}; // 0: not reached
  };
  struct FrameLatency {
    uint64_t frame = 0;
    std::array<int64_t, PointCount> at{}; // nanoseconds, host clock
    std::array<double, StageCount> stage{};
    double toPresent = 0; // FrameStart → PresentEnd, ms
    double toGpuEnd = 0;  // FrameStart → GpuEnd, ms (0 without GPU times)
    bool gpu = false;     // GPU times are part of it
  };
  struct Statistic {
    double last = 0, average = 0, minimum = 0, maximum = 0;
  };
  struct Summary {
    std::vector<FrameLatency> frames; // oldest first
    Statistic toPresent, toGpuEnd;
    std::array<Statistic, StageCount> stage{};
    bool gpu = false; // the frames have GPU times
  };

  inline double milliseconds(int64_t from, int64_t to) {
    return from && to ? std::max(0.0, double(to - from) / 1e6) : 0;
  }

  inline FrameLatency derive(const FrameRecord &record) {
    FrameLatency out;
    out.frame = record.frame;
    out.at = record.at;
    const auto &t = record.at;
    out.stage[Hold] = milliseconds(t[SleepStart], t[FrameStart]);
    out.stage[Simulation] = milliseconds(t[FrameStart], t[PresentCall]);
    out.stage[Handoff] = milliseconds(t[PresentCall], t[SubmitStart]);
    out.stage[Submit] = milliseconds(t[SubmitStart], t[SubmitEnd]);
    out.stage[Queued] = milliseconds(t[SubmitEnd], t[GpuStart]);
    out.stage[Gpu] = milliseconds(t[GpuStart], t[GpuEnd]);
    out.stage[Present] = milliseconds(t[PresentStart], t[PresentEnd]);
    out.toPresent = milliseconds(t[FrameStart], t[PresentEnd]);
    out.gpu = t[GpuStart] && t[GpuEnd];
    out.toGpuEnd = out.gpu ? milliseconds(t[FrameStart], t[GpuEnd]) : 0;
    return out;
  }

  template <class Value> Statistic statistic(const std::vector<FrameLatency> &frames, Value value) {
    Statistic s;
    if (frames.empty())
      return s;
    s.minimum = s.maximum = s.last = value(frames.back());
    double sum = 0;
    for (const auto &frame : frames) {
      const double v = value(frame);
      sum += v;
      s.minimum = std::min(s.minimum, v);
      s.maximum = std::max(s.maximum, v);
    }
    s.average = sum / double(frames.size());
    return s;
  }

  // Thread-safe: marks come from the caller's and the render thread.
  class Recorder {
  public:
    // A point of a frame; a frame number the ring has moved past is dropped.
    void mark(uint64_t frame, Point point, int64_t nanoseconds) {
      if (!frame || point >= PointCount)
        return;
      std::lock_guard lock(m_mutex);
      auto &record = m_ring[frame % ringFrames];
      if (record.frame != frame) {
        if (record.frame > frame)
          return; // older than the frame now in the slot
        record = {};
        record.frame = frame;
      }
      record.at[point] = nanoseconds;
    }
    // Whether frames wait for GPU times before they count (GPU stamps arrive
    // a few frames late; without them, frames count once presented).
    void expectGpu(bool expect) {
      std::lock_guard lock(m_mutex);
      m_expectGpu = expect;
    }
    void reset() {
      std::lock_guard lock(m_mutex);
      m_ring = {};
    }
    // The newest historyFrames complete frames.
    Summary summary() const {
      std::vector<FrameRecord> complete;
      bool gpu;
      {
        std::lock_guard lock(m_mutex);
        gpu = m_expectGpu;
        for (const auto &record : m_ring)
          if (record.frame && record.at[FrameStart] && record.at[PresentCall] && record.at[PresentEnd] &&
              (!gpu || record.at[GpuEnd]))
            complete.push_back(record);
      }
      std::sort(complete.begin(), complete.end(), [](auto &a, auto &b) { return a.frame < b.frame; });
      if (complete.size() > historyFrames)
        complete.erase(complete.begin(), complete.end() - historyFrames);
      Summary out;
      out.gpu = gpu;
      for (const auto &record : complete)
        out.frames.push_back(derive(record));
      out.toPresent = statistic(out.frames, [](const FrameLatency &f) { return f.toPresent; });
      out.toGpuEnd = statistic(out.frames, [](const FrameLatency &f) { return f.toGpuEnd; });
      for (uint32_t s = 0; s < StageCount; ++s)
        out.stage[s] = statistic(out.frames, [s](const FrameLatency &f) { return f.stage[s]; });
      return out;
    }

  private:
    // Room for the history and the frames still waiting for GPU times.
    static constexpr uint32_t ringFrames = 2 * historyFrames;
    mutable std::mutex m_mutex;
    std::array<FrameRecord, ringFrames> m_ring{};
    bool m_expectGpu = false;
  };

  // The summary as lines of text, each starting with prefix (timings.txt, the
  // panel's copy button): last, average, minimum and maximum in milliseconds.
  inline std::string describe(const Summary &s, const char *prefix = "") {
    std::string text;
    char line[200];
    std::snprintf(line, sizeof(line), "%sframe latency over the last %zu frames, ms (last / average / min / max)%s\n",
                  prefix, s.frames.size(), s.gpu ? "" : ", without GPU times");
    text += line;
    auto row = [&](const char *name, const Statistic &v) {
      std::snprintf(line, sizeof(line), "%s  %-28s %7.2f %7.2f %7.2f %7.2f\n", prefix, name, v.last, v.average,
                    v.minimum, v.maximum);
      text += line;
    };
    if (s.gpu)
      row("Frame start to GPU done", s.toGpuEnd);
    row("Frame start to present", s.toPresent);
    for (uint32_t i = 0; i < StageCount; ++i)
      if (s.gpu || (i != Queued && i != Gpu))
        row(stageName(Stage(i)), s.stage[i]);
    return text;
  }

  // GPU timestamps to host nanoseconds through a calibration pair (one GPU
  // tick and one host time read together). validBits masks the counter.
  inline int64_t gpuToHost(uint64_t tick, uint64_t calibrationTick, int64_t calibrationHost, uint32_t validBits,
                           double period) {
    if (!validBits)
      return calibrationHost;
    const uint64_t mask = validBits >= 64 ? UINT64_MAX : (uint64_t(1) << validBits) - 1;
    const uint64_t delta = (tick - calibrationTick) & mask;
    // Ticks before the calibration: the masked difference wrapped.
    int64_t signedDelta;
    if (validBits >= 64)
      signedDelta = int64_t(delta);
    else if (delta & (uint64_t(1) << (validBits - 1)))
      signedDelta = -int64_t((~delta + 1) & mask);
    else
      signedDelta = int64_t(delta);
    return calibrationHost + int64_t(double(signedDelta) * period);
  }
} // namespace remix::latency
