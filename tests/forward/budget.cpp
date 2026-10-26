#include "forward/bake/budget.h"
#include "forward/bake/config.h"
#include "scene/fixtures.h"
#include <limits>

using namespace remix::forward::bake;
using remix::test::require;
int main() {
  for (uint32_t groups : {1u, 65535u, 65536u, 100003u, maximumBatchPoints - 3, maximumBatchPoints}) {
    const auto shape = dispatchShape(groups);
    require(shape.x > 0 && shape.y > 0 && shape.x <= 65535 && shape.y <= 65535 &&
                uint64_t(shape.x) * shape.y >= groups && uint64_t(shape.x) * shape.y - groups < shape.y,
            "Large dispatches fit guaranteed Vulkan dimensions with only a few padded workgroups");
  }
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
    const auto points = budget.count(Work::Visibility, 64, 16, 256, 1, 1000000);
    require(uint64_t(points) * 256 <= Budget::maximumWork && points <= Budget::maximumPoints,
            "Repeated fast samples cannot exceed storage or ray-work limits");
    budget.observe(Work::Visibility, points, .01, 16);
  }
  const auto visibilityLimit = std::min(Budget::maximumPoints, Budget::maximumWork / 256);
  require(budget.count(Work::Visibility, 64, 16, 256, 1, 1000000) == visibilityLimit,
          "Measured fast visibility can fill the point buffer instead of hitting the old 4096-point ceiling");
  const auto &timing = budget.statistics(Work::Visibility);
  require(timing.batches == 30 && timing.points > 0 && timing.milliseconds > 0 &&
              timing.largest == visibilityLimit,
          "Stage diagnostics retain actual GPU work and timestamp totals");
  Budget lowSampleVisibility;
  for (uint32_t iteration = 0; iteration < 30; ++iteration) {
    const auto points = lowSampleVisibility.count(Work::Visibility, 64, 8, 64, 1, 1000000);
    lowSampleVisibility.observe(Work::Visibility, points, .5, 8);
  }
  require(lowSampleVisibility.count(Work::Visibility, 64, 8, 64, 1, 1000000) == Budget::maximumPoints &&
              lowSampleVisibility.count(Work::Visibility, 64, 0, 64, 1, 1000000) == 64,
          "Low-sample visibility can use a larger timed batch while manual execution retains its seed");
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
  require(geometry.count(Work::CoarseTransfer,64,8,1,1,10000) == 64,
          "Coarse transfer starts with independent timing rather than inheriting other geometry costs");
  geometry.observe(Work::CoarseTransfer,64,.01,8);
  require(geometry.count(Work::CoarseTransfer,64,8,1,1,10000) == 128 &&
              geometry.count(Work::Geometry,64,8,1,1,61440) == 61440,
          "Transfer timing cannot perturb ordinary geometry batch scheduling");
  b.batch = 1024;
  b.gpuBudgetMs = 0;
  b.diskCache = false;
  require(a.sameLighting(b) && a != b, "Execution edits preserve the lighting identity");
  b.probeSamples = b.samples;
  require(a.sameLighting(b), "Explicit inherited probe quality preserves lighting identity");
  b.probeSamples *= 4;
  require(!a.sameLighting(b) && a.samples == b.samples,
          "Probe quality can change without raising surface sample counts");
  b.probeSamples = 0;
  b.samples *= 2;
  require(!a.sameLighting(b), "Sample quality still invalidates the bake");
  b = a;
  b.modelLightmaps = true;
  require(!a.sameLighting(b), "Static mesh atlas eligibility changes lighting identity");
}
