#pragma once
#include "forward/probes/capture_work.h"
#include "forward/probes/evaluation.h"
#include "forward/probes/image_asset.h"

namespace remix::forward::probes {
  // Automatic reflection work, independent of diffuse atlases and probe values.
  // Workers advance placement/filtering; the scheduler advances submission and
  // readback counters only after the owning frame fence. No copied ray arrays.
  struct AutomaticCapture {
    void planSites(const scene::BakeScene &source, const bake::Grid &grid,
                    uint32_t count, std::stop_token cancelled);
    void fit(const scene::BakeScene &source, const bake::Grid &grid, uint32_t count,
              float detailTolerance, std::stop_token cancelled);
    void select(const scene::BakeScene &source, uint32_t count, uint32_t resolution,
                 float detailTolerance, std::stop_token cancelled);
    void restoreProjection(const scene::BakeScene &source, float detailTolerance,
                            std::stop_token cancelled);
    void prepareRays(std::stop_token cancelled);
    void complete(std::stop_token cancelled); // retain provenance once, then release ray scratch
    void filter(VkPhysicalDevice physical, VkDevice device, std::stop_token cancelled);
    std::vector<bake::Point> reflectionSites;
    std::vector<uint8_t> usableReflectionSites;
    uint32_t siteNext = 0, siteReceived = 0;
    bool sitesPlanned = false;
    std::vector<FittedRegion> candidateRegions;
    std::unique_ptr<ProjectionAudit> projectionAudit;
    uint32_t geometryNext = 0, geometryReceived = 0;
    bool candidatesPlanned = false;
    std::vector<Capture> captures;
    CaptureWork work;
    uint64_t inputHash = 0;
    Data filtered;
    std::unique_ptr<ImageAsset> image;
    uint32_t captureNext = 0, captureReceived = 0;
    bool capturePlanned = false;
  };
}
