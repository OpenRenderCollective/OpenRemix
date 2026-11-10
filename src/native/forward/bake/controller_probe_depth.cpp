#include "forward/bake/job.h"
#include <cstdio>

namespace remix::forward::bake {
  void Controller::recordProbeDepth(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (job.grid.depth.empty()) {
      job.grid.depth.maximumDistance = Options{}.distance / job.source->sceneScale;
      job.grid.depth.records.resize(job.grid.probes.size());
    }
    if (job.probeDepthNext < job.grid.tasks.size()) {
      auto &submission = m_submissions[m_slot];
      if (submission.job) throw std::logic_error("Probe depth slot reused before its fence");
      const uint32_t count = job.count(Work::ProbeDepth, config,
          uint32_t(job.grid.tasks.size() - job.probeDepthNext), ProbeDepth::texels * ProbeDepth::samples);
      std::vector<Point> points;
      points.reserve(count);
      for (uint32_t i = 0; i < count; ++i)
        points.push_back(job.grid.points[job.grid.tasks[job.probeDepthNext + i]]);
      job.transport->recordProbeDepth(command, m_slot, points);
      submission.job = m_job;
      submission.probeDepth = true;
      submission.first = job.probeDepthNext;
      submission.count = count;
      job.probeDepthNext += count;
    }
    char progress[128];
    std::snprintf(progress, sizeof(progress), "Baking directional probe visibility (%u/%zu probes)",
                  job.probeDepthReceived, job.grid.tasks.size());
    status(REMIXAPI_BAKE_TRACING_EXT, progress);
  }
}
