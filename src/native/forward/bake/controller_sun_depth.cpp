#include "forward/bake/job.h"
#include "forward/bake/sun_depth.h"
#include <cstdio>
namespace remix::forward::bake {
  bool Controller::recordSunDepth(VkCommandBuffer command, const Config &config) {
    auto &job = *m_job;
    if (job.sunDepth && !job.sunDepth->restored() && job.sunDepthReceived < job.sunDepth->tasks()) {
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
    if (job.sunDepth && !job.sunDepth->restored() && job.sunDepth->cacheEnabled() && !job.sunDepthCacheWritten) {
      if (!job.sunDepthCacheSubmitted) {
        auto &submission = m_submissions[m_slot];
        if (submission.job) throw std::logic_error("Sun depth readback slot reused before its fence");
        job.sunDepth->recordReadback(command, m_physical, m_device);
        submission.job = m_job;
        submission.sunDepthCache = true;
        job.sunDepthCacheSubmitted = true;
      } else if (job.sunDepthCacheReady) {
        auto owner = m_job;
        static std::atomic<uint64_t> cacheGeneration = 0;
        job.ticket = m_pipelines->prepare("forward-sun-depth-cache-" + std::to_string(++cacheGeneration), [owner] {
          owner->sunDepth->storeCache(owner->cancellation.get_token());
          owner->sunDepthCacheWritten = true;
        });
      }
      status(REMIXAPI_BAKE_PREPARING_EXT, "Saving sparse sun depth cache");
      return true;
    }
    if (job.sunDepth && !job.sunDepthLogged) {
      std::fprintf(stderr, "Forward sparse sun depth complete: %u rays, %.3f ms GPU\n", job.sunDepthReceived, job.sunDepthGpuMs);
      job.sunDepthLogged = true;
    }
    return false;
  }
}
