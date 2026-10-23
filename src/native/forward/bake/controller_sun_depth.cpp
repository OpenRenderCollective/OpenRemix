#include "forward/bake/job.h"
#include "forward/bake/sun_depth.h"
#include <cstdio>
namespace remix::forward::bake {
  bool Controller::recordSunDepth(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (job.sunDepth && job.sunDepthReceived < job.sunDepth->tasks()) {
      if (job.sunDepthNext < job.sunDepth->tasks()) {
        auto &submission = m_submissions[m_slot];
        if (submission.job) throw std::logic_error("Sun depth slot reused before its fence");
        const uint32_t count = job.count(Work::Geometry, config, job.sunDepth->tasks() - job.sunDepthNext, 1);
        const auto rays = job.sunDepth->batch(job.sunDepthNext, count);
        job.transport->recordIndexedDepth(command, m_slot, rays, job.sunDepth->values(), job.sunDepth->distance());
        submission.job = m_job;
        submission.sunDepth = true;
        submission.first = job.sunDepthNext;
        submission.count = count;
        job.sunDepthNext += count;
      }
      char progress[128];
      std::snprintf(progress, sizeof(progress), "Baking sparse sun depth (%u/%u rays)", job.sunDepthReceived, job.sunDepth->tasks());
      status(REMIXAPI_BAKE_TRACING_EXT, progress);
      return true;
    }
    if (job.sunDepth && !job.sunDepthLogged) {
      std::fprintf(stderr, "Forward sparse sun depth complete: %u rays, %.3f ms GPU\n", job.sunDepthReceived, job.sunDepthGpuMs);
      job.sunDepthLogged = true;
    }
    return false;
  }
}
