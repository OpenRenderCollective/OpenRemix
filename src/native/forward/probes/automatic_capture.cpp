#include "forward/probes/automatic_capture.h"
#include "forward/probes/capture_dump.h"
#include "forward/probes/placement.h"
#include "platform/workers.h"
#include "scene/types.h"
#include <cstring>

namespace remix::forward::probes {
  void AutomaticCapture::select(const scene::BakeScene &source, uint32_t count, uint32_t resolution,
                                 float detailTolerance, std::stop_token cancelled) {
    requirePreparing();
    capturePlanned = true;
    if (projectionAudit) {
      candidateRegions = projectionAudit->finish();
      projectionAudit.reset();
    }
    captures = captureCandidates(source, candidateRegions, count, resolution, cancelled, detailTolerance);
    dumpCandidates(source, candidateRegions, captures, cancelled);
  }
  void AutomaticCapture::restoreProjection(const scene::BakeScene &source, float detailTolerance,
                                            std::stop_token cancelled) {
    requirePreparing();
    // Derived overlap may change without invalidating captured radiance.
    configureInfluence(source, captures, detailTolerance, cancelled);
    for (size_t i = 0; i < captures.size(); ++i)
      filtered.header.probes[i] = projectionRecord(captures[i]);
  }
  void AutomaticCapture::prepareRays(std::stop_token cancelled) {
    requirePreparing();
    std::vector<FittedRegion>().swap(candidateRegions);
    seeds = {}; // release spatial inputs before allocating capture rays
    work = CaptureWork(captures, cancelled);
    if (!captures.empty())
      inputHash = scene::fastHash(work.rays().data(), work.rays().size() * sizeof(bake::Point)) | 1;
  }
  void AutomaticCapture::restoreCapture(std::vector<Capture> cached, Data data, std::stop_token cancelled) {
    requirePreparing();
    if (cached.size() != captures.size() || data.header.parameters[0] != captures.size()) return;
    for (size_t i = 0; i < captures.size(); ++i) {
      const auto expected = projectionInputRecord(captures[i]);
      auto actual = data.header.probes[i];
      actual.position[3] = 0; // captured confidence is output, not projection input
      if (!sameCaptureRays(captures[i], cached[i]) || std::memcmp(&expected, &actual, sizeof(expected))) return;
    }
    // Legacy raw captures omit explicit feather distance; retain the newly fitted
    // metadata and reuse only transport. The filtered header was checked above.
    for (size_t i = 0; i < captures.size(); ++i)
      captures[i].radiance = std::move(cached[i].radiance);
    filtered = std::move(data);
    inputHash = CaptureWork::inputHash(captures, cancelled);
    cachedRadiance = true;
    std::vector<FittedRegion>().swap(candidateRegions);
    seeds = {};
    work = {}; // completed cached radiance needs no GPU capture submissions
  }
  void AutomaticCapture::complete(std::stop_token cancelled) {
    requirePreparing();
    if (cancelled.stop_requested()) throw std::runtime_error("Reflection capture completion superseded");
    if (!cachedRadiance) work.complete(captures, cancelled);
    work = {};
  }
  void AutomaticCapture::filter(VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled) {
    requirePreparing();
    if (!cachedRadiance) {
      WorkerPool workers;
      filtered = probes::filter(captures, 128, cancelled, &workers);
    }
    image = std::make_shared<ImageAsset>(physical, device, filtered, cancelled);
  }
}
