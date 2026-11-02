#include "forward/probes/automatic_capture.h"
#include "forward/probes/capture_dump.h"
#include "forward/probes/placement.h"
#include "platform/workers.h"
#include "scene/types.h"

namespace remix::forward::probes {
  void AutomaticCapture::planSites(const scene::BakeScene &source, const bake::Grid &grid,
                                    uint32_t count, std::stop_token cancelled) {
    sitesPlanned = true;
    const auto sites = candidateSites(source, grid, count, cancelled);
    reflectionSites.reserve(sites.size());
    for (const auto &position : sites)
      reflectionSites.push_back({{position[0], position[1], position[2], 0}, {}});
    usableReflectionSites.resize(sites.size());
  }
  void AutomaticCapture::fit(const scene::BakeScene &source, const bake::Grid &grid, uint32_t count,
                              float detailTolerance, std::stop_token cancelled) {
    candidatesPlanned = true;
    std::vector<Vec3> validated;
    for (size_t i = 0; i < reflectionSites.size(); ++i)
      if (usableReflectionSites[i]) {
        const auto &position = reflectionSites[i].position;
        validated.push_back({position[0], position[1], position[2]});
      }
    auto regions = fitCandidates(source, grid, count, cancelled, detailTolerance, &validated);
    if (source.partition && source.partition->closed && !regions.empty())
      projectionAudit = std::make_unique<ProjectionAudit>(std::move(regions), cancelled);
    else
      candidateRegions = std::move(regions);
  }
  void AutomaticCapture::select(const scene::BakeScene &source, uint32_t count, uint32_t resolution,
                                 float detailTolerance, std::stop_token cancelled) {
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
    // Derived overlap may change without invalidating captured radiance.
    configureInfluence(source, captures, detailTolerance, cancelled);
    for (size_t i = 0; i < captures.size(); ++i)
      filtered.header.probes[i] = projectionRecord(captures[i]);
  }
  void AutomaticCapture::prepareRays(std::stop_token cancelled) {
    std::vector<FittedRegion>().swap(candidateRegions);
    work = CaptureWork(captures, cancelled);
  }
  void AutomaticCapture::complete(std::stop_token cancelled) {
    work.complete(captures, cancelled);
    if (!captures.empty())
      inputHash = scene::fastHash(work.rays().data(),
          work.rays().size() * sizeof(bake::Point)) | 1;
    work = {};
  }
  void AutomaticCapture::filter(VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled) {
    {
      WorkerPool workers;
      filtered = probes::filter(captures, 128, cancelled, &workers);
    }
    image = std::make_shared<ImageAsset>(physical, device, filtered, cancelled);
  }
}
