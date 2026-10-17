#include "forward/probes/evaluation.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace remix::forward;
namespace p = remix::forward::probes;
namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  Bounds box(Vec3 lo, Vec3 hi) {
    Bounds b;
    b.include(lo);
    b.include(hi);
    return b;
  }
  // Independent analytic empty-room union. Merge ray/box intervals to find
  // the first solid wall, including passage through the low doorway.
  double wallDistance(const bake::Point &ray, std::span<const Bounds> rooms) {
    std::vector<std::pair<double, double>> intervals;
    for (const auto &room : rooms) {
      double first = -1e30, last = 1e30;
      for (uint32_t axis = 0; axis < 3; ++axis) {
        const double d = ray.normal[axis], o = ray.position[axis];
        if (std::abs(d) < 1e-10) {
          if (o < room.minimum[axis] || o > room.maximum[axis])
            last = -1e30;
        } else {
          const double a = (room.minimum[axis] - o) / d, b = (room.maximum[axis] - o) / d;
          first = std::max(first, std::min(a, b));
          last = std::min(last, std::max(a, b));
        }
      }
      if (first <= last && last >= 0)
        intervals.emplace_back(first, last);
    }
    std::sort(intervals.begin(), intervals.end());
    double exit = 0;
    for (const auto &[first, last] : intervals) {
      if (first > exit + 1e-7)
        break;
      exit = std::max(exit, last);
    }
    require(exit > 0, "Analytic geometry starts in an empty room");
    return exit;
  }
  std::vector<bake::Result> geometry(const p::ProjectionAudit &audit, std::span<const Bounds> rooms) {
    std::vector<bake::Result> values(audit.rays().size());
    for (size_t i = 0; i < values.size(); ++i) {
      values[i].irradiance[0][3] = float(wallDistance(audit.rays()[i], rooms));
      values[i].statistics[2] = 1;
    }
    return values;
  }
} // namespace
int main() {
  const Bounds rooms[] = {box({-10, -2, 0}, {-.1f, 2, 2.5f}), box({.1f, -2, 0}, {10, 2, 2.5f}),
                          box({-.1f, -1.75f, 0}, {.1f, 1.75f, 1.5f})};
  const std::vector<p::FittedRegion> candidates = {
      {box({-9.999f, -1.749f, .001f}, {9.999f, 1.749f, 1.499f}), {{0, 0, .75f}, .1f, 3}},
      {box({-9.999f, -1.999f, .001f}, {-.101f, 1.999f, 2.499f}), {{-5, 0, 1.75f}, .1f, 3}},
      {box({.101f, -1.999f, .001f}, {9.999f, 1.999f, 2.499f}), {{5, 0, 1.75f}, .1f, 3}}};
  require(p::selectRegions(candidates, 1) == std::vector<size_t>{0},
          "The larger empty doorway proxy reproduces the volume-only "
          "allocation problem");
  p::ProjectionAudit audit(candidates);
  require(audit.rays().size() == 3 * 6 * 8 * 8,
          "Candidate geometry work has a fixed bounded directional set");
  auto values = geometry(audit, rooms);
  bool incomplete = false;
  try {
    audit.finish();
  } catch (const std::logic_error &) {
    incomplete = true;
  }
  require(incomplete, "Incomplete geometry cannot choose capture slots");
  auto invalid = values;
  invalid.back().statistics[1] = std::numeric_limits<float>::quiet_NaN();
  require(!audit.observe(0, invalid), "Invalid flags refuse the entire geometry batch");
  require(audit.observe(0, values), "A valid batch remains usable after an invalid batch was refused");
  require(!audit.observe(0, values), "Duplicate readback cannot bias geometry preference");
  const auto assessed = audit.finish();
  const auto selected = p::selectRegions(assessed, 2);
  require(selected == std::vector<size_t>({1, 2}) && assessed[1].quality > assessed[0].quality + .2f,
          "Measured geometry selects both full-height rooms instead of the "
          "larger low doorway proxy");
  p::ProjectionAudit streamed(candidates);
  const size_t half = values.size() / 2;
  require(streamed.observe(uint32_t(half), std::span(values).subspan(half)) &&
              streamed.observe(0, std::span(values).first(half)),
          "Completed frame slots may deliver geometry out of order");
  const auto reordered = streamed.finish();
  for (size_t i = 0; i < assessed.size(); ++i)
    require(std::abs(reordered[i].quality - assessed[i].quality) < 1e-6f,
            "Geometry preference is independent of batch boundaries and "
            "completion order");
  p::ProjectionAudit misses(candidates);
  auto missValues = values;
  for (auto &value : missValues)
    value.statistics[1] = 1;
  require(misses.observe(0, missValues), "Distance-cap misses retain their explicit flags");
  for (const auto &region : misses.finish())
    require(region.quality == .05f && std::abs(region.missFraction - 1) < 1e-6f,
            "A miss cannot masquerade as a supporting wall even at an "
            "identical reported distance");
  auto scaled = candidates;
  for (auto &region : scaled) {
    for (auto *v : {&region.bounds.minimum, &region.bounds.maximum, &region.site.position})
      for (auto &axis : *v)
        axis *= 40;
  }
  p::ProjectionAudit units(scaled);
  auto scaledValues = values;
  for (auto &value : scaledValues)
    value.irradiance[0][3] *= 40;
  require(units.observe(0, scaledValues), "Scaled geometry queries are valid");
  const auto unitRegions = units.finish();
  for (size_t i = 0; i < assessed.size(); ++i)
    require(std::abs(unitRegions[i].quality - assessed[i].quality) < 1e-6f,
            "Candidate geometry agreement is independent of world units");
  p::ProjectionAudit empty({});
  require(empty.rays().empty() && empty.finish().empty(), "Empty candidate work needs no GPU submission");
  bool bounded = false;
  try {
    p::ProjectionAudit excessive(std::vector<p::FittedRegion>(161));
  } catch (const std::invalid_argument &) {
    bounded = true;
  }
  require(bounded, "Candidate limits are checked before ray allocation");
  std::stop_source stop;
  stop.request_stop();
  bool cancelled = false;
  try {
    p::ProjectionAudit stopped(candidates, stop.get_token());
  } catch (const std::runtime_error &) {
    cancelled = true;
  }
  require(cancelled, "Map changes cancel geometry preparation before allocation");
  std::printf("Candidate geometry: doorway %.4f, rooms %.4f/%.4f; bounded "
              "streaming and explicit misses passed\n",
              assessed[0].quality, assessed[1].quality, assessed[2].quality);
}
