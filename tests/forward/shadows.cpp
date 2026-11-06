#include "forward/shadows/plan.h"
#include "forward/shadows/moving_cache.h"
#include "scene/fixtures.h"
#include <cmath>

using namespace remix;
using namespace remix::forward;
using namespace remix::test;
namespace s = remix::forward::shadows;
void movingCacheTests(const s::View &view) {
  s::MovingCache cache;
  auto surface = std::make_shared<scene::Surface>();
  std::vector<s::MovingCaster> casters{{surface, {}, scene::identity(), 0, 3, 0, true}};
  require(cache.update(view, casters) && !cache.update(view, casters),
          "Opaque moving maps render once and reuse identical caster state");
  auto changedView = view;
  changedView.position[0] += 1;
  require(cache.update(changedView, casters) && cache.update(view, casters),
          "Light view changes invalidate moving depth independently of intensity/fades");
  changedView = view;
  changedView.matrices[0][0] += .125f;
  require(cache.update(changedView, casters) && cache.update(view, casters),
          "Projection matrices invalidate depth even when light identity/range match");
  casters[0].model[12] += 1;
  require(cache.update(view, casters), "Moving geometry transforms invalidate depth");
  ++casters[0].firstIndex;
  require(cache.update(view, casters), "Index range changes invalidate depth");
  ++casters[0].indexCount;
  require(cache.update(view, casters), "Index count changes invalidate depth");
  --casters[0].vertexOffset;
  require(cache.update(view, casters), "Base vertex changes invalidate depth");
  std::vector<Matrix> pose{scene::identity()};
  casters[0].bones = std::make_shared<const std::vector<Matrix>>(pose);
  require(cache.update(view, casters), "Introducing a skin invalidates depth");
  casters[0].bones = std::make_shared<const std::vector<Matrix>>(pose);
  require(!cache.update(view, casters), "Equal independently submitted poses reuse depth");
  pose[0][12] = 2;
  casters[0].bones = std::make_shared<const std::vector<Matrix>>(pose);
  require(cache.update(view, casters), "Changed bone matrices invalidate depth");
  casters[0].bones.reset();
  require(cache.update(view, casters), "Removing skinning invalidates depth");
  auto second = std::make_shared<scene::Surface>();
  casters.push_back({second, {}, scene::identity(), 0, 3, 0, true});
  require(cache.update(view, casters), "Caster membership invalidates moving maps");
  std::swap(casters[0], casters[1]);
  require(cache.update(view, casters), "Changed source order is conservatively redrawn");
  second.reset();
  require(cache.update(view, casters) && cache.update(view, casters),
          "Expired geometry never reuses a stale depth image");
  casters.resize(1);
  casters[0].surface = surface;
  casters[0].opaque = false;
  require(cache.update(view, casters) && cache.update(view, casters),
          "Texture-dependent alpha coverage always updates");
  scene::Material material;
  material.texture = std::make_shared<scene::Texture>();
  DrawData data{};
  data.tint = {1, 1, 1, 1}; data.uv = {1, 1, 0, 0};
  data.state[1] = 6;
  auto prepareAlpha = [&] { casters[0].alpha = s::alphaCoverage(data, material, 17); };
  prepareAlpha();
  require(cache.update(view, casters) && !cache.update(view, casters),
          "Immutable alpha texels and exact prepared state permit reuse");
  data.uv[2] = .5f; prepareAlpha();
  require(cache.update(view, casters), "Animated atlas coordinates invalidate alpha depth");
  data.tint[3] = .4f; prepareAlpha();
  require(cache.update(view, casters), "Alpha tint changes invalidate depth");
  data.factor[3] = .3f; prepareAlpha();
  require(cache.update(view, casters), "Texture factor changes invalidate depth");
  data.state[2] = 1; prepareAlpha();
  require(cache.update(view, casters), "Alpha reference changes invalidate depth");
  data.maps[0] = 32; prepareAlpha();
  require(cache.update(view, casters), "Colour decode policy invalidates alpha composition");
  casters[0].alpha.sampler = 18;
  require(cache.update(view, casters), "Sampler changes invalidate alpha coverage");
  ++material.wrapU; prepareAlpha();
  require(cache.update(view, casters), "Changed wrap mode invalidates a recycled sampler handle");
  ++material.filter; prepareAlpha();
  require(cache.update(view, casters), "Changed filter invalidates a recycled sampler handle");
  material.texture = std::make_shared<scene::Texture>(); prepareAlpha();
  require(cache.update(view, casters), "Replaced texels invalidate alpha depth");
  material.texture->updates = std::make_shared<scene::TextureUpdates>(); prepareAlpha();
  require(cache.update(view, casters) && cache.update(view, casters),
          "In-place texel updates always retain live rendering");
  material.texture->updates.reset(); material.texture->dynamic = true; prepareAlpha();
  require(cache.update(view, casters) && cache.update(view, casters),
          "Dynamic texture storage never uses CPU revisions as GPU proof");
  material.texture->dynamic = false;
  material.texture->encodedSource = std::make_shared<scene::Texture>();
  material.texture->encodedSource->updates = std::make_shared<scene::TextureUpdates>(); prepareAlpha();
  require(cache.update(view, casters) && cache.update(view, casters),
          "Updatable encoded source views retain live rendering");
  casters[0].opaque = true;
  require(cache.update(view, casters) && !cache.update(view, casters),
          "An uncacheable frame invalidates prior opaque contents");
  cache.reset();
  require(cache.update(view, casters), "Reactivated slots never inherit stale contents");
  require(cache.update(view, {}) && !cache.update(view, {}),
          "Leaving casters clear a map once before empty reuse");
  casters.resize(s::MovingCache::maximumCasters + 1, casters[0]);
  require(cache.update(view, casters), "Oversized caster lists retain ordinary rendering");
  casters.resize(1);
  casters[0].bones = std::make_shared<const std::vector<Matrix>>(s::MovingCache::maximumPoseMatrices + 1);
  require(cache.update(view, casters), "Pose retention has a bounded matrix budget");
}
int main() {
  scene::SphereLight light;
  light.id = 10;
  light.position[0] = 2;
  light.position[1] = -3;
  light.position[2] = 4;
  light.radius = .25f;
  light.radiance[0] = 1;
  const auto view = s::lightView(light, 10, 1);
  movingCacheTests(view);
  const Vec3 directions[] = {{1, -.3f, -.7f}, {-1, -.3f, .7f}, {.7f, 1, .3f},
                             {.7f, -1, -.3f}, {.7f, -.3f, 1},  {-.7f, -.3f, -1}};
  for (uint32_t face = 0; face < s::faces; ++face) {
    Vec3 p;
    for (uint32_t c = 0; c < 3; ++c)
      p[c] = view.position[c] + directions[face][c] * 2;
    const auto projected = transformPoint(view.matrices[face], p);
    require(std::abs(projected[0] - .7f) < 1e-5f && std::abs(projected[1] - .3f) < 1e-5f &&
                projected[2] > 0 && projected[2] < 1,
            "Six perspective faces match the reflection cube convention at translated light origins");
  }
  auto empty = s::emptyCoverage();
  for (const auto &rectangle : empty)
    require(rectangle[0] > rectangle[2] && rectangle[1] > rectangle[3], "Empty caster coverage is explicit");
  auto unknown = empty;
  s::includeCoverage(unknown, {}, view);
  for (const auto &rectangle : unknown)
    require(rectangle == Vec4{-1, -1, 1, 1}, "Unknown caster bounds retain every moving-shadow face");
  Vec4 projectedCoverage{1, 1, -1, -1};
  Bounds orthographic;
  orthographic.include({-.2f, -.4f, .3f});
  orthographic.include({.3f, .2f, .5f});
  s::includeCoverage(projectedCoverage, orthographic, scene::identity());
  require(projectedCoverage[0] <= -.2f && projectedCoverage[1] <= -.4f &&
              projectedCoverage[2] >= .3f && projectedCoverage[3] >= .2f,
          "Orthographic caster coverage conservatively contains its projected corners");
  s::includeCoverage(projectedCoverage, {}, scene::identity());
  require(projectedCoverage == Vec4{-1, -1, 1, 1}, "Unknown sun casters retain full filtering");
  auto narrow = scene::identity();
  narrow[0] = 1024;
  narrow[12] = -1048576;
  Bounds distant;
  distant.include({1024, 0, .25f});
  distant.include({1024.0005f, .1f, .5f});
  projectedCoverage = {1, 1, -1, -1};
  s::includeCoverage(projectedCoverage, distant, narrow);
  require(projectedCoverage[0] < 0 && projectedCoverage[2] > .5f,
          "Narrow sun projections cover float transform cancellation at distant origins");
  Bounds bounds;
  bounds.include({2, -3, 3});
  bounds.include({2, -3, 5});
  require(s::intersects(bounds, view), "Light containing a caster bound is retained");
  bounds = {};
  bounds.include({50, 50, 50});
  require(!s::intersects(bounds, view), "Distant casters outside the radial range are rejected");
  for (uint32_t face = 0; face < s::faces; ++face) {
    require(intersectsClip({}, view.matrices[face]), "Unknown caster bounds survive every shadow face");
    Bounds nearLight;
    for (uint32_t c = 0; c < 3; ++c) {
      nearLight.minimum[c] = view.position[c] - 1;
      nearLight.maximum[c] = view.position[c] + 1;
    }
    nearLight.valid = true;
    require(intersectsClip(nearLight, view.matrices[face]),
            "A caster enclosing the light survives all faces even without an interior corner");
    for (uint32_t direction = 0; direction < s::faces; ++direction) {
      Bounds axial;
      for (uint32_t c = 0; c < 3; ++c) {
        const float centre = view.position[c] + (c == direction / 2 ? (direction % 2 ? -2.f : 2.f) : 0);
        axial.minimum[c] = centre - .1f;
        axial.maximum[c] = centre + .1f;
      }
      axial.valid = true;
      require(intersectsClip(axial, view.matrices[face]) == (face == direction),
              "An axial caster is submitted only to its own translated cube face");
      auto coverage = s::emptyCoverage();
      s::includeCoverage(coverage, axial, view);
      if (face != direction) {
        require(coverage[face][0] > coverage[face][2], "An absent caster leaves face coverage empty");
      } else {
        for (uint32_t corner = 0; corner < 8; ++corner) {
          Vec3 point;
          for (uint32_t axis = 0; axis < 3; ++axis)
            point[axis] = corner & (1u << axis) ? axial.maximum[axis] : axial.minimum[axis];
          const auto projected = transformPoint(view.matrices[face], point);
          for (uint32_t axis = 0; axis < 2; ++axis)
            require(projected[axis] >= coverage[face][axis] && projected[axis] <= coverage[face][axis + 2],
                    "Projected caster corners stay within the conservative face union");
        }
      }
    }
    const uint32_t axis = face / 2;
    const float sign = face % 2 ? -1.f : 1.f;
    auto atDepth = [&](float depth, float extent) {
      Bounds result;
      result.valid = true;
      for (uint32_t c = 0; c < 3; ++c) {
        const float centre = view.position[c] + (c == axis ? sign * depth : 0);
        result.minimum[c] = centre - extent;
        result.maximum[c] = centre + extent;
      }
      return result;
    };
    require(!intersectsClip(atDepth(12, .1f), view.matrices[face]),
            "A caster wholly beyond the face far plane is rejected");
    require(intersectsClip(atDepth(10, .1f), view.matrices[face]) &&
                intersectsClip(atDepth(view.nearPlane, .0005f), view.matrices[face]),
            "Casters crossing the near or far plane remain submitted");
  }
  // Check all cube seams/corners against directly projected points. The box
  // may span faces even when its centre belongs to a different face.
  for (int x = -4; x <= 4; ++x)
    for (int y = -4; y <= 4; ++y)
      for (int z = -4; z <= 4; ++z) {
        Bounds box;
        const Vec3 centre{view.position[0] + x, view.position[1] + y, view.position[2] + z};
        for (uint32_t c = 0; c < 3; ++c) {
          box.minimum[c] = centre[c] - .25f;
          box.maximum[c] = centre[c] + .25f;
        }
        box.valid = true;
        for (uint32_t face = 0; face < s::faces; ++face) {
          auto coverage = s::emptyCoverage();
          s::includeCoverage(coverage, box, view);
          bool containsProjectedPoint = false;
          for (uint32_t corner = 0; corner < 8; ++corner) {
            Vec4 projected{};
            for (uint32_t row = 0; row < 4; ++row) {
              projected[row] = view.matrices[face][12 + row];
              for (uint32_t c = 0; c < 3; ++c)
                projected[row] += view.matrices[face][c * 4 + row] *
                    (corner & (1u << c) ? box.maximum[c] : box.minimum[c]);
            }
            const float w = projected[3];
            const bool visible = w > 0 && std::abs(projected[0]) <= w &&
                std::abs(projected[1]) <= w && projected[2] >= 0 && projected[2] <= w;
            containsProjectedPoint |= visible;
            if (visible)
              for (uint32_t axis = 0; axis < 2; ++axis) {
                const float coordinate = projected[axis] / w;
                require(coordinate >= coverage[face][axis] && coordinate <= coverage[face][axis + 2],
                        "Caster coverage retains visible seam/corner points and near-plane crossing boxes");
              }
          }
          require(!containsProjectedPoint || intersectsClip(box, view.matrices[face]),
                  "A shadow face never culls a box with a raster-visible seam or corner point");
        }
      }

  NativeResources resources;
  const auto mesh = quad(resources, 801, material(resources, 800, 1, 1, 1));
  remixapi_InstanceIdentityEXT identity{};
  identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
  identity.instanceId = 802;
  identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
  identity.rasterVisible = 1;
  auto info = instance(mesh);
  info.pNext = &identity;
  const auto retained = resources.createRetained(info);
  auto source = std::make_shared<scene::BakeScene>();
  source->ready = true;
  source->draws = resources.retained(retained)->records;
  scene::freezeBakeScene(*source);
  s::Planner planner;
  auto selected = planner.update({&light, 1}, {}, 1, .001f, 100, 1, source);
  const auto slot = selected.slots[0];
  require(slot == 0 && selected.dirty[slot], "First map is dirty");
  require(planner.captured(resources.retained(retained)->records[0]),
          "Explicit immutable membership is recognized");
  {
    auto receiver = std::make_shared<scene::BakeScene>(*source);
    receiver->receiverDraws = {1};
    s::Planner liveReceiver;
    liveReceiver.update({&light, 1}, {}, 1, .001f, 100, 1, receiver);
    require(!liveReceiver.captured(resources.retained(retained)->records[0]),
            "Receiver chart ownership never classifies a live caster as baked transport");
  }
  auto draw = resources.retained(retained)->records[0];
  draw.instanceId++;
  require(!planner.captured(draw), "A settled actor sharing geometry is still a live caster");
  draw = resources.retained(retained)->records[0];
  draw.model[12] += 1;
  require(!planner.captured(draw), "A changed transform cannot reuse frozen membership");
  draw = resources.retained(retained)->records[0];
  draw.blend.factor = 0;
  require(!planner.captured(draw), "A changed legacy material factor cannot reuse static opacity");
  selected = planner.update({&light, 1}, {}, 1, .001f, 100, 1, source);
  require(!selected.dirty[slot], "Unchanged authored geometry and light reuse the static map");
  auto moved = light;
  moved.position[0] += 1;
  selected = planner.update({&moved, 1}, {}, 1, .001f, 100, 1, source);
  require(selected.dirty[slot], "Moving the emitter invalidates its cached projection");
  auto replacement = std::make_shared<scene::BakeScene>(*source);
  selected = planner.update({&moved, 1}, {}, 1, .001f, 100, 1, replacement);
  require(selected.dirty[slot], "Replacing the captured scene invalidates all static maps");
  selected = planner.update({&moved, 1}, {}, 0, .001f, 100, 1, replacement);
  require(selected.slots[0] == UINT32_MAX && selected.overflow == 1,
          "Zero budget keeps the light and explicitly reports the unshadowed fallback");
  scene::SphereLight pair[] = {light, moved};
  pair[1].id = 20;
  selected = planner.update(pair, {}, 2, .001f, 100, 1, replacement);
  const auto oldSlot = selected.slots[0];
  std::swap(pair[0], pair[1]);
  selected = planner.update(pair, {}, 2, .001f, 100, 1, replacement);
  require(selected.slots[1] == oldSlot && !selected.dirty[oldSlot],
          "Submission order and priority changes preserve surviving cache slots");
  selected = planner.update(pair, {}, 2, .001f, 100, 1, {});
  require(selected.dirty[oldSlot] && !planner.captured(resources.retained(retained)->records[0]),
          "Unload clears captured membership and invalidates cached occluders");
  {
    s::Planner fading;
    scene::SphereLight lights[] = {light, light};
    lights[0].id = 101;
    lights[1].id = 102;
    lights[0].position[0] = 0;
    lights[1].position[0] = 10;
    auto at = [&](double time, bool second) {
      return fading.update(lights, {second ? 10.f : 0.f, -3, 4}, 1, .001f, 100, 1, source, time, 2);
    };
    auto frame = at(0, false);
    require(frame.slots[0] == 0 && frame.strength[0] == 0 && frame.dirty[0],
            "A new selection prepares its map at zero shadow strength");
    frame = at(1, false);
    require(frame.strength[0] == .5f && !frame.dirty[0], "Fade follows elapsed seconds without redrawing static maps");
    frame = at(2, false);
    require(frame.strength[0] == 1, "Incoming shadow reaches full strength after two seconds");
    frame = at(2, true);
    require(frame.slots[0] == 0 && frame.slots[1] == UINT32_MAX && frame.strength[0] == 1,
            "Priority changes preserve the outgoing map until it fades");
    frame = at(3, true);
    require(frame.strength[0] == .5f && frame.views[0].id == 101 && !frame.dirty[0],
            "Outgoing shadow fades using its own cached map");
    frame = at(3.5, false);
    require(frame.strength[0] == .75f, "Priority reversal recovers smoothly from the current strength");
    frame = at(3, false);
    require(frame.strength[0] == .75f, "A backward clock cannot reverse or advance a fade");
    frame = at(4, true);
    require(frame.strength[0] == .5f, "Clock recovery counts only new elapsed time");
    frame = at(5, true);
    require(frame.slots[0] == UINT32_MAX && frame.slots[1] == 0 && frame.strength[0] == 0 && frame.dirty[0],
            "Only a fully faded slot can be reused for a different light");
    frame = at(6, true);
    require(frame.strength[0] == .5f && frame.views[0].id == 102 && !frame.dirty[0],
            "Replacement shadow fades in independently with a valid new map");
    frame = at(7, true);
    require(frame.strength[0] == 1, "Replacement finishes its own two-second fade");
    std::swap(lights[0], lights[1]);
    frame = at(7, true);
    require(frame.slots[0] == 0 && frame.strength[0] == 1, "Submission reordering preserves transition identity");
    frame = fading.update(lights, {}, 0, .001f, 100, 1, source, 8, 2);
    require(!frame.active[0] && frame.slots[0] == UINT32_MAX, "Disabling shadows releases slots immediately");
    frame = fading.update(lights, {10, -3, 4}, 1, .001f, 100, 1, source, 8, 0);
    require(frame.slots[0] == 0 && frame.strength[0] == 1, "Zero fade duration retains immediate selection");
    frame = fading.update({}, {}, 1, .001f, 100, 1, source, 9, 2);
    require(!frame.active[0], "Removed or zero-influence lights cannot retain a stale shadow assignment");
  }
  {
    s::Planner fading;
    scene::SphereLight lights[] = {light, light, light};
    lights[0].id = lights[1].id = 301;
    lights[2].id = 302;
    lights[2].position[0] += 10;
    const Vec3 eye{lights[0].position[0], lights[0].position[1], lights[0].position[2]};
    const Vec3 moved{eye[0] + 10, eye[1], eye[2]};
    auto frame = fading.update(lights, eye, 1, .001f, 100, 1, source, 0, 2);
    require(frame.slots[0] == 0 && frame.slots[1] == UINT32_MAX,
            "Equal shadow candidates sharing an ID retain their input order");
    fading.update(lights, eye, 1, .001f, 100, 1, source, 2, 2);
    lights[1].radiance[0] *= 2;
    frame = fading.update(lights, moved, 1, .001f, 100, 1, source, 3, 2);
    require(frame.slots[1] == 0 && frame.slots[0] == UINT32_MAX && frame.slots[2] == UINT32_MAX &&
                frame.strength[0] == .5f,
            "An outgoing slot retains the best occurrence of its ID outside the ranked prefix");
  }
  {
    scene::SphereLight lights[] = {light, light};
    lights[0].id = 201;
    lights[0].radiance[0] = 100;
    lights[1].id = 202;
    lights[1].radiance[0] = 1;
    const uint8_t fallback[] = {1, 0};
    s::Planner priority;
    auto frame = priority.update(lights, {}, 1, .001f, 100, 1, source);
    require(frame.slots[0] == 0, "Legacy importance ranking selects the brighter light");
    frame = priority.update(lights, {}, 1, .001f, 100, 1, source, 0, 0, fallback);
    require(frame.slots[1] == 0 && frame.slots[0] == UINT32_MAX,
            "Mixed shadows take priority over brighter fully baked fallback emitters");
    frame = priority.update(lights, {}, 2, .001f, 100, 1, source, 0, 0, fallback);
    require(frame.slots[0] != UINT32_MAX && frame.slots[1] != UINT32_MAX,
            "Spare slots retain fully baked light shadows for uncovered receivers");
    const auto retainedSlots = frame.slots;
    bool rejected = false;
    try { priority.update(lights, {}, 1, .001f, 100, 1, source, 0, 0, std::span(fallback).first(1)); }
    catch (const std::invalid_argument &) { rejected = true; }
    frame = priority.update(lights, {}, 2, .001f, 100, 1, source, 0, 0, fallback);
    require(rejected && frame.slots == retainedSlots && !frame.dirty[0] && !frame.dirty[1],
            "A mismatched fallback policy is rejected before changing cache ownership");
    const uint8_t onlyFallback[] = {1, 1};
    frame = priority.update(lights, {}, 1, .001f, 100, 1, source, 0, 0, onlyFallback);
    require(frame.slots[0] == 0 && frame.slots[1] == UINT32_MAX,
            "An all-baked scene retains its ordinary ranked fallback shadow coverage");
    s::Planner fading;
    frame = fading.update(lights, {}, 1, .001f, 100, 1, source, 0, 2);
    frame = fading.update(lights, {}, 1, .001f, 100, 1, source, 2, 2);
    require(frame.slots[0] == 0 && frame.strength[0] == 1, "Outgoing fallback shadow starts fully visible");
    frame = fading.update(lights, {}, 1, .001f, 100, 1, source, 3, 2, fallback);
    require(frame.slots[0] == 0 && frame.slots[1] == UINT32_MAX && frame.strength[0] == .5f,
            "A mixed-priority change fades the old map using its own identity");
    frame = fading.update(lights, {}, 1, .001f, 100, 1, source, 4, 2, fallback);
    require(frame.slots[0] == UINT32_MAX && frame.slots[1] == 0 && frame.strength[0] == 0,
            "Mixed replacement acquires a slot only after fallback fade-out");
    frame = fading.update(lights, {}, 1, .001f, 100, 1, source, 5, 2, fallback);
    require(frame.slots[1] == 0 && frame.strength[0] == .5f && !frame.dirty[0],
            "Mixed replacement fades in without redrawing unchanged static geometry");
  }
  std::puts("Local shadow cube projections, cache ownership, selection and caster policy passed");
}
