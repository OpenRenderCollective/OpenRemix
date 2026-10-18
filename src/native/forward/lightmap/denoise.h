#pragma once
#include "forward/lightmap/layout.h"
#include <functional>
#include <span>

namespace remix::forward::lightmap {
  // Zero means this build has no OIDN support. Included in the bake cache key.
  uint32_t denoiserVersion();
  enum class HostTransfers { Pageable, Pinned };
  enum class FilterDispatch { Automatic, Ordinary };
  struct DenoiseStatistics {
    uint32_t charts = 0;
    uint64_t texels = 0;
    uint32_t executions = 0, preparations = 0, constantFields = 0, monitoredExecutions = 0;
    uint32_t batches = 0; // one ordered upload/filter/readback packet per nonconstant batch
    uint32_t synchronizations = 0;
    uint32_t graphCaptures = 0, graphLaunches = 0, graphFallbacks = 0;
    uint64_t peakHostBytes = 0, imageBufferBytes = 0, transferBytes = 0;
    uint64_t stagingBufferBytes = 0;
    int deviceType = 0; // OIDNDeviceType; no OIDN types escape this module
    double seconds = 0;
    // Host wall intervals, not GPU timestamps. Submission may block in the
    // driver; waits measure only explicit synchronization. These intervals do
    // not include one another and leave orchestration as the unmeasured rest.
    double initializationSeconds = 0, preparationSeconds = 0;
    double uploadSeconds = 0, executionSeconds = 0, readbackSeconds = 0;
    double waitSeconds = 0, chartSeconds = 0;
    double stagingSeconds = 0;
  };
  // Worker-only offline operation, after transport readback and before atlas encoding.
  // Filters indirect RGB SH within each chart. Signed bands are normalized by L0,
  // filtered as directional data, then reconstructed with the filtered L0.
  // Alpha, visibility, direct-light attribution, probe data and moments are untouched.
  // Exceptions abort publication. OIDN 2.5 images share its native 16-pixel network
  // padding; signed -1 and HDR zero preserve zero tensor boundaries. Equal-shaped
  // images can queue independent host packets, bounded by 32 charts and an 8 MiB
  // scratch target (one oversized chart remains valid). Cancellation is checked
  // between batches and enqueues; queued work is drained before cleanup. CPU and large GPU images
  // additionally use OIDN's internal cancellation callback.
  // GPU transfers use an additional pinned arena capped at 8 MiB, reused only
  // after synchronization. On Windows, the default CUDA device can reuse one
  // captured kernel packet per current binding/active-band mask. Graphs retire
  // before rebinding; transfers remain ordered outside capture. Explicit device
  // selection, other backends/platforms, monitored fields and unavailable graph
  // support retain ordinary dispatch. This changes submission, not filtering.
  // CPU devices, oversized packets and failed pinned
  // allocations retain pageable transfers. Pageable mode supports comparisons.
  DenoiseStatistics denoise(const Layout &layout, std::span<bake::Result> results,
                            std::stop_token cancelled = {},
                            std::function<void(uint32_t completed, uint32_t total)> progress = {},
                            HostTransfers transfers = HostTransfers::Pinned,
                            FilterDispatch dispatch = FilterDispatch::Automatic);
} // namespace remix::forward::lightmap
