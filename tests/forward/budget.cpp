#include "forward/bake/budget.h"
#include "forward/bake/config.h"
#include "scene/fixtures.h"
#include <limits>

using namespace remix::forward::bake;
using remix::test::require;
int main() {
  Budget budget;
  require(budget.count(Work::Irradiance, 64, 2, 256, 3, 10000) == 64,
          "Automatic work starts from the configured seed");
  budget.observe(Work::Irradiance, 64, .125, 2);
  require(budget.count(Work::Irradiance, 64, 2, 256, 3, 10000) == 128,
          "Headroom increases work cautiously instead of jumping to an unmeasured maximum");
  require(budget.count(Work::Visibility, 64, 2, 256, 1, 10000) == 64 &&
              budget.count(Work::Surface, 64, 2, 256, 3, 10000) == 64,
          "Surface, visibility and grid transport learn independent costs");
  budget.observe(Work::Irradiance, 128, 8, 2);
  require(budget.count(Work::Irradiance, 64, 2, 256, 3, 10000) == Budget::minimumAutomaticPoints,
          "A slow batch reduces work while preserving the minimum automatic GPU occupancy");
  const auto learned = budget.count(Work::Irradiance, 64, 2, 256, 3, 10000);
  budget.observe(Work::Irradiance, 1, .001, 2);
  budget.observe(Work::Irradiance, 100, std::numeric_limits<double>::quiet_NaN(), 2);
  budget.observe(Work::Irradiance, 100, 0, 2);
  require(budget.count(Work::Irradiance, 64, 2, 256, 3, 10000) == learned,
          "Small tails and invalid timing cannot corrupt the estimate");
  require(budget.count(Work::Irradiance, 17, 0, 256, 3, 10000) == 17 &&
              budget.count(Work::Irradiance, 17, 0, 8192, 8, 10000) == 16 &&
              budget.count(Work::Sun, 64, 2, 256, 1, 0) == 0,
          "Manual work, maximum-quality work ceilings and exhausted queues stay bounded");
  for (uint32_t iteration = 0; iteration < 30; ++iteration) {
    const auto points = budget.count(Work::Visibility, 64, 16, 256, 1, 100000);
    require(uint64_t(points) * 256 <= Budget::maximumWork && points <= Budget::maximumPoints,
            "Repeated fast samples cannot exceed storage or ray-work limits");
    budget.observe(Work::Visibility, points, .01, 16);
  }
  require(budget.count(Work::Visibility, 64, 16, 256, 1, 100000) == Budget::maximumPoints,
          "Measured fast visibility can fill the point buffer instead of hitting the old 4096-point ceiling");
  const auto &timing = budget.statistics(Work::Visibility);
  require(timing.batches == 30 && timing.points > 0 && timing.milliseconds > 0 &&
              timing.largest == Budget::maximumPoints,
          "Stage diagnostics retain actual GPU work and timestamp totals");
  Config a, b;
  require(budget.count(Work::Reflection, 1024, 0, 8192, 8, 5000) == 16 &&
              budget.count(Work::Reflection, 1024, 8, 8192, 8, 5000) <= 256,
          "Reflection capture quality obeys the same manual and timed ray-work ceilings");
  Budget geometry;
  for (uint32_t iteration = 0; iteration < 12; ++iteration) {
    const auto points = geometry.count(Work::Geometry, 64, 8, 1, 1, 61440);
    require(points <= 61440 && points <= Budget::maximumPoints && points <= Budget::maximumWork,
            "Candidate geometry obeys storage, remaining-work and ray-work bounds");
    geometry.observe(Work::Geometry, points, .01, 8);
  }
  require(geometry.count(Work::Geometry, 64, 8, 1, 1, 61440) == 61440,
          "Fast geometry queries can fill the bounded candidate set without "
          "inheriting lighting sample cost");
  b.batch = 1024;
  b.gpuBudgetMs = 0;
  b.diskCache = false;
  require(a.sameLighting(b) && a != b, "Execution edits preserve the lighting identity");
  b.samples *= 2;
  require(!a.sameLighting(b), "Sample quality still invalidates the bake");
}
