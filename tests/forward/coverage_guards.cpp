#include "forward/probes/coverage_guards.h"
#include <limits>
#include <stdexcept>

namespace p = remix::forward::probes;
using remix::forward::Vec3;
void require(bool value) { if (!value) throw std::runtime_error("Coverage guard invariant failed"); }
int main() {
  p::CoverageVolume volume;
  volume.record.influenceMinimum = {-5, -10, -15, 0};
  volume.record.influenceMaximum = {5, 10, 15, 0};
  volume.record.position = {1, 2, 3, 0};
  std::vector<p::CoverageVolume> volumes{volume};
  auto points = p::coverageGuardPoints(volumes);
  require(points.size() == 126 && points[0] == Vec3{1, 2, 3});
  require(points[1] == Vec3{-4, -8, -12} && points[2] == Vec3{-4, -8, -6});
  require(points[6] == Vec3{-4, -4, -12} && points[26] == Vec3{-2, -8, -12});
  require(points[63] == Vec3{0, 0, 0} && points.back() == Vec3{4, 8, 12});
  volume.record.worldToLocal = {{{0, 1, 0, -20}, {-1, 0, 0, 10}, {0, 0, 1, -30}}};
  volume.record.position = {10, 20, 30, 0};
  volumes.push_back(volume);
  points = p::coverageGuardPoints(volumes);
  require(points.size() == 252 && points[126] == Vec3{10, 20, 30});
  require(points[127] == Vec3{18, 16, 18} && points[189] == Vec3{10, 20, 30} &&
          points.back() == Vec3{2, 24, 42});
  auto rejects = [](auto action) {
    try { action(); } catch (const std::exception &) { return true; }
    return false;
  };
  volumes[0].record.position[0] = std::numeric_limits<float>::infinity();
  require(rejects([&] { p::coverageGuardPoints(volumes); }));
  volumes[0] = volume;
  volumes[0].record.influenceMaximum[0] = -5;
  require(rejects([&] { p::coverageGuardPoints(volumes); }));
  volumes[0] = volume;
  volumes[0].record.worldToLocal[0][0] = 4;
  require(rejects([&] { p::coverageGuardPoints(volumes); }));
  volumes.assign(65, volume);
  require(rejects([&] { p::coverageGuardPoints(volumes); }));
  std::stop_source stop; stop.request_stop();
  require(rejects([&] { p::coverageGuardPoints({}, stop.get_token()); }));
  require(p::coverageGuardPoints({}).empty());
  p::ReceiverFits fits;
  p::ReceiverProjection variant;
  variant.influence.valid = true;
  variant.influence.minimum = {-10, -10, -10};
  variant.influence.maximum = {10, 10, 10};
  variant.geometry.emplace();
  variant.geometry->geometryEvaluated = true;
  variant.geometry->site.position = {0, 0, 5};
  fits.attempts.push_back({0, {variant, variant}});
  std::vector<Vec3> guards{{0, 0, 0}, {0, 0, 5}, {50, 0, 0}, {0, 0, 4.999f}, {1, 0, 0}};
  const auto pairs = p::coverageGuardPairs(guards, fits, .01f);
  require(pairs.size() == 2 && pairs[0].receiver == 0 && pairs[1].receiver == 4 && pairs[0].capture == 0);
  require(std::abs(pairs[0].ray.distance - 5.f) < 1e-5 &&
          pairs[0].ray.point.position[2] == 0 && pairs[0].ray.point.normal[2] == 1);
  const auto bounded = p::coverageGuardPairs(guards, fits, .01f, 1);
  require(bounded.size() == 1 && bounded[0].receiver == 4);
  require(rejects([&] { p::coverageGuardPairs(guards, fits, .01f, 0); }));
  require(rejects([&] { p::coverageGuardPairs(guards, fits, .01f, 1, stop.get_token()); }));
  fits.attempts[0].variants[1].geometry->site.position[0] = 1;
  require(rejects([&] { p::coverageGuardPairs(guards, fits, .01f); }));
  fits.attempts[0].variants = {variant};
  guards[0][0] = std::numeric_limits<float>::quiet_NaN();
  require(rejects([&] { p::coverageGuardPairs(guards, fits, .01f); }));
}
