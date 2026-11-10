#include "forward/probes/automatic_capture.h"
#include "forward/probes/capture_dump.h"
#include "forward/probes/placement.h"
#include <algorithm>

namespace remix::forward::probes {
AutomaticCapture::Step AutomaticCapture::prepareNext(
    const scene::BakeScene &source, const Placement &placement,
    std::stop_token cancelled, std::optional<PlacementSeeds> admittedSeeds) {
  requirePreparing();
  if (cancelled.stop_requested())
    throw std::runtime_error("Reflection preparation superseded");
  const auto step = nextStep();
  if (admittedSeeds && step != Step::PlanSites)
    throw std::logic_error("Reflection seeds supplied after site planning");
  switch (step) {
  case Step::PlanSites:
    if (admittedSeeds)
      planSites(source, std::move(*admittedSeeds), placement.requested,
                cancelled);
    else
      planSites(source, placement.requested, cancelled);
    break;
  case Step::Fit:
    fit(source, placement.requested, placement.detailTolerance, cancelled);
    break;
  case Step::Select:
    select(source, std::min(placement.requested, placement.capacity),
           placement.resolution, placement.detailTolerance, cancelled);
    break;
  default:
    throw std::logic_error("Reflection preparation requires retired GPU work");
  }
  return step;
}
void AutomaticCapture::select(const scene::BakeScene &source, uint32_t count,
                              uint32_t resolution, float detailTolerance,
                              std::stop_token cancelled) {
  requirePreparing();
  capturePlanned = true;
  if (projectionAudit) {
    candidateRegions = projectionAudit->finish();
    projectionAudit.reset();
  }
  captures = captureCandidates(source, candidateRegions, count, resolution,
                               cancelled, detailTolerance);
  dumpCandidates(source, candidateRegions, captures, cancelled);
}
} // namespace remix::forward::probes
