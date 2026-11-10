#pragma once
#include "forward/probes/capture_work.h"
#include "forward/probes/coverage_image_plan.h"

namespace remix::forward::probes {
  struct LocalCaptureImage {
    Data data;
    CoverageReadyImage selection;
  };
  // Bounded radiance preparation for an already selected coverage plan. The
  // render controller submits rays and calls observe only after each frame fence.
  class LocalCaptureWork {
  public:
    static constexpr uint32_t maximumCaptures = 4;
    LocalCaptureWork(const scene::BakeScene &source, const CoverageBaselineSet &baseline,
        const ReceiverCoveragePlan &plan, uint32_t resolution, float blendDistance = 0,
        std::stop_token cancelled = {});
    std::span<const bake::Point> rays() const { return m_work.rays(); }
    bool observe(uint32_t first, std::span<const bake::Result> results);
    bool complete() const { return m_finished || m_received == m_seen.size(); }
    LocalCaptureImage finish(uint32_t filterSamples = 128, std::stop_token cancelled = {}, WorkerPool *workers = nullptr);
  private:
    CoveragePlanContext m_context;
    std::vector<CoverageReadyLayer> m_layers;
    std::vector<Capture> m_captures;
    CaptureWork m_work;
    std::vector<uint8_t> m_seen;
    size_t m_received = 0;
    bool m_finished = false;
  };
}
