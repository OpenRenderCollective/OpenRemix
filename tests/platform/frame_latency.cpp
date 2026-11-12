#include "platform/frame_latency.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  bool near(double a, double b) { return std::abs(a - b) < 1e-9; }
  constexpr int64_t ms = 1000000; // nanoseconds

  // A frame whose points are base + the given milliseconds.
  void frame(remix::latency::Recorder &recorder, uint64_t id, int64_t base, bool gpu = true) {
    using namespace remix::latency;
    recorder.mark(id, SleepStart, base);
    recorder.mark(id, FrameStart, base + 2 * ms);
    recorder.mark(id, PresentCall, base + 5 * ms);
    recorder.mark(id, SubmitStart, base + 6 * ms);
    recorder.mark(id, SubmitEnd, base + 7 * ms);
    recorder.mark(id, PresentStart, base + 8 * ms);
    recorder.mark(id, PresentEnd, base + 9 * ms);
    if (gpu) {
      recorder.mark(id, GpuStart, base + 10 * ms);
      recorder.mark(id, GpuEnd, base + 14 * ms);
    }
  }
} // namespace

int main() {
  using namespace remix::latency;
  {
    // Stages and totals of one frame.
    Recorder recorder;
    recorder.expectGpu(true);
    frame(recorder, 1, 1000 * ms);
    const auto s = recorder.summary();
    require(s.gpu && s.frames.size() == 1, "One complete frame");
    const auto &f = s.frames[0];
    require(near(f.stage[Hold], 2) && near(f.stage[Simulation], 3) && near(f.stage[Handoff], 1) &&
                near(f.stage[Submit], 1) && near(f.stage[Queued], 3) && near(f.stage[Gpu], 4) &&
                near(f.stage[Present], 1),
            "Stage durations");
    require(near(f.toGpuEnd, 12) && near(f.toPresent, 7), "Frame start to GPU end and to present");
  }
  {
    // Frames wait for GPU times when they are expected, and count without them otherwise.
    Recorder recorder;
    recorder.expectGpu(true);
    frame(recorder, 1, 0 + ms, false);
    require(recorder.summary().frames.empty(), "A frame without GPU times waits for them");
    recorder.mark(1, GpuStart, 11 * ms);
    recorder.mark(1, GpuEnd, 15 * ms);
    require(recorder.summary().frames.size() == 1, "GPU times arriving late complete the frame");
    recorder.expectGpu(false);
    frame(recorder, 2, 100 * ms, false);
    const auto s = recorder.summary();
    require(!s.gpu && s.frames.size() == 2, "Without GPU times, presented frames count");
    require(near(s.frames[1].toGpuEnd, 0) && !s.frames[1].gpu, "No GPU total without GPU times");
  }
  {
    // The history keeps the newest frames in order; statistics over them.
    Recorder recorder;
    recorder.expectGpu(true);
    for (uint64_t id = 1; id <= 200; ++id)
      frame(recorder, id, int64_t(id) * 20 * ms);
    // A slower newest frame: its GPU ends 10 ms later.
    recorder.mark(201, FrameStart, 5000 * ms);
    recorder.mark(201, PresentCall, 5001 * ms);
    recorder.mark(201, PresentEnd, 5002 * ms);
    recorder.mark(201, GpuStart, 5003 * ms);
    recorder.mark(201, GpuEnd, 5022 * ms);
    const auto s = recorder.summary();
    require(s.frames.size() == historyFrames, "History length");
    require(s.frames.front().frame == 201 - historyFrames + 1 && s.frames.back().frame == 201,
            "Newest frames, in order");
    require(near(s.toGpuEnd.last, 22) && near(s.toGpuEnd.maximum, 22) && near(s.toGpuEnd.minimum, 12),
            "Last, minimum and maximum");
    require(near(s.toGpuEnd.average, (12.0 * (historyFrames - 1) + 22) / historyFrames), "Average");
    // A late mark for a frame the ring has moved past is dropped.
    recorder.mark(201 - 2 * historyFrames, GpuEnd, 1);
    require(recorder.summary().frames.back().frame == 201, "Stale marks do not displace newer frames");
    recorder.mark(0, FrameStart, 1);
    require(recorder.summary().frames.size() == historyFrames, "Frame 0 is never recorded");
  }
  {
    // GPU ticks to host time: before and after the calibration, and a narrow counter's wrap.
    require(gpuToHost(1100, 1000, 5 * ms, 64, 10) == 5 * ms + 1000, "Ticks after the calibration");
    require(gpuToHost(900, 1000, 5 * ms, 64, 10) == 5 * ms - 1000, "Ticks before the calibration");
    // 32 valid bits: the counter wrapped between the calibration and the stamp.
    require(gpuToHost(5, 0xFFFFFFF0u, 5 * ms, 32, 1) == 5 * ms + 21, "Counter wrap after the calibration");
    require(gpuToHost(0xFFFFFFF0u, 5, 5 * ms, 32, 1) == 5 * ms - 21, "Counter wrap before the calibration");
    require(gpuToHost(123, 0, 7, 0, 1) == 7, "No valid bits");
  }
  {
    // The text form names every shown stage.
    Recorder recorder;
    recorder.expectGpu(false);
    frame(recorder, 1, ms, false);
    const auto text = describe(recorder.summary(), "# ");
    require(text.find("Frame start to present") != std::string::npos &&
                text.find("Frame start to GPU done") == std::string::npos &&
                text.find("GPU render") == std::string::npos && text.find("Low latency hold") != std::string::npos,
            "Text without GPU times");
  }
  std::puts("frame latency ok");
  return 0;
}
