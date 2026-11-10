#include "forward/probes/automatic_capture.h"
#include "forward/probes/cache_index.h"
#include <cstdio>

namespace remix::forward::probes {
uint64_t AutomaticCapture::prepareRadiance(const CaptureConfig &config,
                                           const capture_cache::Planned &cache,
                                           std::stop_token cancelled) {
  requirePreparing();
  if (nextStep() != Step::Capture && nextStep() != Step::Complete)
    throw std::logic_error(
        "Reflection radiance preparation requires selected captures");
  if (cachedRadiance || !work.rays().empty() || captureNext || captureReceived)
    throw std::logic_error("Reflection radiance was already prepared");
  uint64_t identity = 0;
  if (!cache.directory.empty() && !captureView().empty()) {
    identity = capture_cache::plannedKey(
        cache.content, cache.shaders,
        CaptureWork::planHash(captureView(), cancelled), captureView(), config,
        cache.device, cache.lightingComponents, cache.directOwnership);
    auto cached = cache_index::read(cache.directory, identity,
                                    cache.device.limits, cancelled);
    const bool indexed = bool(cached);
    if (cached)
      restoreCapture(std::move(cached->captures), std::move(cached->data),
                     cancelled);
    std::fprintf(stderr,
                 "Forward reflection cache: key=%llu indexed=%u reused=%u\n",
                 static_cast<unsigned long long>(identity), unsigned(indexed),
                 unsigned(cachedRadiance));
  }
  if (!cachedRadiance)
    prepareRays(cancelled);
  return identity;
}
} // namespace remix::forward::probes
