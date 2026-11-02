#include "forward/probes/automatic_capture.h"
#include "forward/bake/budget.h"
#include "forward/bake/transport.h"
#include <stdexcept>

namespace remix::forward::probes {
  bake::Work AutomaticCapture::workType(Step step) {
    switch (step) {
    case Step::ValidateSites: return bake::Work::Site;
    case Step::CheckGeometry: return bake::Work::Geometry;
    case Step::Capture: return bake::Work::Reflection;
    default: throw std::logic_error("Reflection timing requires a GPU query stage");
    }
  }
  AutomaticCapture::Dispatch AutomaticCapture::record(VkCommandBuffer command, uint32_t slot,
      bake::Transport &transport, bake::Budget &budget, const CaptureConfig &config) {
    requirePreparing();
    const auto step = nextStep();
    const float milliseconds = transport.timingSupported() ? config.gpuBudgetMs : 0;
    auto count = [&](bake::Work kind, uint32_t remaining, uint32_t samples, uint32_t bounces = 1) {
      return budget.count(kind, config.batch, milliseconds, samples, bounces, remaining);
    };
    switch (step) {
    case Step::ValidateSites: {
      Dispatch dispatch{step, siteNext};
      if (siteNext >= reflectionSites.size()) return dispatch; // readback pending
      dispatch.count = count(bake::Work::Site, uint32_t(reflectionSites.size() - siteNext), siteSamples);
      transport.recordSiteValidation(command, slot,
          std::span(reflectionSites).subspan(siteNext, dispatch.count), siteSamples);
      siteNext += dispatch.count;
      return dispatch;
    }
    case Step::CheckGeometry: {
      Dispatch dispatch{step, geometryNext};
      const auto rays = projectionAudit->rays();
      if (geometryNext >= rays.size()) return dispatch;
      dispatch.count = count(bake::Work::Geometry, uint32_t(rays.size() - geometryNext), 1);
      transport.recordGeometry(command, slot, rays.subspan(geometryNext, dispatch.count));
      geometryNext += dispatch.count;
      return dispatch;
    }
    case Step::Capture: {
      Dispatch dispatch{step, captureNext};
      const auto rays = work.rays();
      if (captureNext >= rays.size()) return dispatch;
      dispatch.count = count(bake::Work::Reflection, uint32_t(rays.size() - captureNext),
                               config.samples, config.bounces);
      transport.recordRadiance(command, slot, rays.subspan(captureNext, dispatch.count),
                                config.bounces, config.samples);
      captureNext += dispatch.count;
      return dispatch;
    }
    default:
      throw std::logic_error("Automatic capture dispatch requires a query phase");
    }
  }
}
