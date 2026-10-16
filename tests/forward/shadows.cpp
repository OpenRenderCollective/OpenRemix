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
  std::puts("Local shadow cube projections, cache ownership, selection and caster policy passed");
}
