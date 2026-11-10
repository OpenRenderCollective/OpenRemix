#include "forward/probes/coverage_selection.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using namespace remix::forward::probes;
namespace {
  void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
  }
}
int main() {
  const std::vector<float> areas{1, 1, 5, 2, 3};
  const std::vector<CoverageBaseline> baseline{{{0,1},false}, {{0,1},true}};
  const std::vector<CoverageCandidate> candidates{{{2},.1f}, {{3},.05f}, {{2,4},.3f}};
  const auto replacement = planCoverageReplacements(areas, baseline, candidates, .2f, 2);
  require(replacement.size() == 1 && replacement[0].candidate == 0 && replacement[0].baseline == 0 &&
          replacement[0].gainedArea == 5, "Replace redundant automatic coverage while protecting authored coverage");
  const auto free = planCoverageReplacements(areas, baseline, candidates, .2f, 4);
  require(free.size() == 2 && free[0].baseline == UINT32_MAX && free[1].baseline == UINT32_MAX,
          "Use free layers before displacing any baseline capture");
  const std::vector<CoverageBaseline> unique{{{0,1},false}};
  require(planCoverageReplacements(areas, unique, candidates, .2f, 1).empty(),
          "New samples cannot compensate for losing previously covered samples");
  const std::vector<CoverageCandidate> superset{{{0,1,2},.1f}};
  const auto supersetPlan = planCoverageReplacements(areas, unique, superset, .2f, 1);
  require(supersetPlan.size() == 1 && supersetPlan[0].gainedArea == 5,
          "A replacement may preserve a baseline's unique samples itself");
  const std::vector<CoverageBaseline> shared{{{0},false}, {{0},false}};
  require(planCoverageReplacements(areas, shared, candidates, .2f, 2).size() == 1,
          "Recompute unique ownership after each swap instead of removing both shared owners");
  const std::vector<CoverageBaseline> protectedOnly{{{0},true}};
  require(planCoverageReplacements(areas, protectedOnly, superset, .2f, 1).empty(),
          "Protected captures remain even when a candidate contains their sampled footprint");
  const std::vector<CoverageCandidate> tied{{{2},.1f}, {{2},.05f}};
  const auto tie = planCoverageReplacements(areas, {}, tied, .2f, 1);
  require(tie.size() == 1 && tie[0].candidate == 1, "Equal area gains prefer smaller projection error");
  const std::vector<float> guardedWeights{1, 0, 5};
  const std::vector<CoverageBaseline> guardedBaseline{{{0,1},false}, {{0},false}};
  const std::vector<CoverageCandidate> guardedCandidate{{{2},.1f}};
  const auto guarded = planCoverageReplacements(guardedWeights, guardedBaseline, guardedCandidate, .2f, 2);
  require(guarded.size() == 1 && guarded[0].baseline == 1 && guarded[0].gainedArea == 5,
          "Zero-weight volume guards prevent a surface-preserving swap from losing spatial coverage");
  const std::vector<CoverageCandidate> guardOnly{{{1},.1f}};
  require(planCoverageReplacements(guardedWeights, {}, guardOnly, .2f, 1).empty(),
          "Covering empty-space guards alone cannot earn a new capture layer");
  bool invalid = false;
  const std::vector<CoverageCandidate> duplicate{{{2,2},.1f}};
  try { (void)planCoverageReplacements(areas, {}, duplicate, .2f); }
  catch (const std::invalid_argument &) { invalid = true; }
  require(invalid, "Duplicate sample identities cannot inflate coverage or corrupt ownership");
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try { (void)planCoverageReplacements(areas, baseline, candidates, .2f, 64, 4, stop.get_token()); }
  catch (const std::runtime_error &) { cancelled = true; }
  require(cancelled, "Superseded coverage plans stop before doing work");
  std::puts("Bounded reflection coverage replacement preserves samples and protected captures");
}
