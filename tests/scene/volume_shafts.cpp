#include "pathtrace/volume_policy.h"
#include "scene/fixtures.h"
#include <cmath>
#include <vector>

// remixapi_DrawVolumeShaftsEXT: validation, the orthonormal frame made of what
// the host gave, publication for one frame, and the traced view's choice and
// packing of the nearest (pathtrace/volume_policy.h).
using namespace remix::test;
namespace {
  bool close(float a, float b) { return std::abs(a - b) <= 1e-4f * std::max(1.f, std::max(std::abs(a), std::abs(b))); }

  remixapi_VolumeShaftEXT shaft(float x) {
    remixapi_VolumeShaftEXT s = {};
    s.sType = REMIXAPI_STRUCT_TYPE_VOLUME_SHAFT_EXT;
    s.origin = {x, 0, 10};
    s.axis = {0, 0, -10}; // straight down, 10 long
    s.halfU = {2, 0, 1};  // tilted along the axis: only its part across counts
    s.halfV = {1, 3, 0};  // not perpendicular to u: made so
    s.farScaleU = 2;
    s.farScaleV = 1;
    s.radiance = {.5f, .4f, .3f};
    s.farFade = 0;
    s.softness = 2; // clamped to 1
    s.shape = REMIXAPI_VOLUME_SHAFT_BOX_EXT;
    return s;
  }

  float dot(const std::array<float, 3> &a, const std::array<float, 3> &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  }
} // namespace

int main() {
  using namespace remix;
  NativeResources api, frame;
  camera(api);
  const remixapi_VolumeShaftEXT shafts[] = {shaft(0), shaft(100), shaft(5)};
  api.drawVolumeShafts(shafts, 3);
  require(api.volumeShafts().size() == 3, "Three shafts drawn");
  const auto &s = api.volumeShafts()[0];
  require(close(s.length, 10) && close(s.axis[2], -1), "The axis is normalised, its length kept");
  require(close(dot(s.u, s.axis), 0) && close(dot(s.v, s.axis), 0) && close(dot(s.u, s.v), 0) &&
              close(dot(s.u, s.u), 1) && close(dot(s.v, s.v), 1),
          "u, v and the axis are orthonormal");
  require(close(s.nearHalf[0], 2) && close(s.nearHalf[1], 3) && close(s.farHalf[0], 4) && close(s.farHalf[1], 3),
          "The half extents across the axis, the far cap scaled");
  require(close(s.softness, 1), "Softness is clamped");

  // An elliptical shaft tapers uniformly: by the mean of the two scales.
  auto round = shaft(0);
  round.shape = REMIXAPI_VOLUME_SHAFT_ELLIPSE_EXT;
  api.drawVolumeShafts(&round, 1);
  require(api.volumeShafts().size() == 1 && close(api.volumeShafts()[0].farHalf[0], 3) &&
              close(api.volumeShafts()[0].farHalf[1], 4.5f),
          "A cone's far scale is the mean, and a new call replaces the frame's shafts");

  // Degenerate shafts are left out; malformed ones are refused.
  auto flat = shaft(0);
  flat.halfV = {0, 0, 5}; // along the axis: no width
  auto dark = shaft(0);
  dark.radiance = {0, 0, 0};
  const remixapi_VolumeShaftEXT degenerate[] = {flat, dark};
  api.drawVolumeShafts(degenerate, 2);
  require(api.volumeShafts().empty(), "A shaft with no width or no glow is left out");
  bool refused = false;
  auto wrong = shaft(0);
  wrong.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  try {
    api.drawVolumeShafts(&wrong, 1);
  } catch (...) {
    refused = true;
  }
  require(refused, "A shaft of the wrong type is refused");
  refused = false;
  auto negative = shaft(0);
  negative.farFade = -1;
  try {
    api.drawVolumeShafts(&negative, 1);
  } catch (...) {
    refused = true;
  }
  require(refused, "A negative fade is refused");
  refused = false;
  auto unknown = shaft(0);
  unknown.flags = 2;
  try {
    api.drawVolumeShafts(&unknown, 1);
  } catch (...) {
    refused = true;
  }
  require(refused, "Unknown flags are refused");

  // Published with the frame, gone from the next one.
  api.drawVolumeShafts(shafts, 3);
  api.publishFrame(frame);
  require(frame.volumeShafts().size() == 3, "The shafts are published with the frame");
  require(api.volumeShafts().empty(), "The next API frame starts with none");
  frame.finishFrame();
  require(frame.volumeShafts().empty(), "A finished frame keeps none");

  // The traced view: the nearest first, at most volumeShaftSlots.
  std::vector<VolumeShaft> many;
  for (int i = 0; i < 40; ++i) {
    VolumeShaft v;
    v.origin = {float(100 - i * 2), 0, 0};
    v.axis = {0, 0, 1};
    v.u = {1, 0, 0};
    v.v = {0, 1, 0};
    v.length = 1;
    v.nearHalf = v.farHalf = {.5f, .5f};
    many.push_back(v);
  }
  const float eye[3] = {}, forward[3] = {};
  const auto chosen = trace::selectVolumeShafts(many, eye, forward);
  require(chosen.size() == trace::volumeShaftSlots && chosen.front() == 39 && chosen.back() == 8,
          "The nearest 32 are drawn, nearest first");
  // Behind the eye along a direction given: left out.
  const float ahead[3] = {-1, 0, 0};
  require(trace::selectVolumeShafts(many, eye, ahead).empty(), "Shafts wholly behind the eye are left out");

  // Packing: v is flipped so that cross(u, v) is the axis.
  VolumeShaft packed = many[0];
  packed.v = {0, -1, 0};
  packed.shape = REMIXAPI_VOLUME_SHAFT_ELLIPSE_EXT;
  packed.flags = REMIXAPI_VOLUME_SHAFT_SHADOWED_EXT;
  float out[5][4];
  trace::packVolumeShaft(packed, out);
  require(close(out[0][3], 1) && close(out[2][1], 1) && close(out[1][3], .5f), "Packed with cross(u, v) the axis");
  require(close(out[4][3], 3), "Shape and flags packed as shape + 2 x flags");
  return 0;
}
