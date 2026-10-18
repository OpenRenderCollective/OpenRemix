#include "forward/shadows/plan.h"
#include "scene/fixtures.h"
#include <cmath>

using namespace remix;
using namespace remix::forward;
using namespace remix::test;
namespace s = remix::forward::shadows;
int main() {
  scene::SphereLight light;
  light.id = 10;
  light.position[0] = 2;
  light.position[1] = -3;
  light.position[2] = 4;
  light.radius = .25f;
  light.radiance[0] = 1;
  const auto view = s::lightView(light, 10, 1);
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
  Bounds bounds;
  bounds.include({2, -3, 3});
  bounds.include({2, -3, 5});
  require(s::intersects(bounds, view), "Light containing a caster bound is retained");
  bounds = {};
  bounds.include({50, 50, 50});
  require(!s::intersects(bounds, view), "Distant casters outside the radial range are rejected");

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
