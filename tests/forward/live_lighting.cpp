#include "forward/options.h"
#include "forward/shadows/plan.h"
#include "forward/shadows/static_casters.h"
#include "forward/sky_model.h"
#include "forward/half.h"
#include "scene/fixtures.h"
#include <cmath>

using namespace remix;
using namespace remix::forward;
using namespace remix::test;
namespace s = remix::forward::shadows;

int main() {
  {
    // The dome frame is Y-up for every world up axis, with handedness kept.
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const auto toDome = sky_model::worldToDome(axis);
      Vec3 up{};
      up[axis] = 1;
      const auto mapped = transformVector(toDome, up);
      require(std::abs(mapped[1] - 1) < 1e-6f && std::abs(mapped[0]) < 1e-6f && std::abs(mapped[2]) < 1e-6f,
              "The world up axis maps to the dome's +Y");
      const auto x = transformVector(toDome, {1, 0, 0}), y = transformVector(toDome, {0, 1, 0}),
                 z = transformVector(toDome, {0, 0, 1});
      require(std::abs(dot(cross(x, y), z) - 1) < 1e-6f, "Dome mappings are rotations");
    }
    scene::DistantLight scene;
    scene.isDistant = true;
    scene.direction[0] = .3f;
    scene.direction[1] = -.9f;
    scene.radiance[0] = 2;
    Options options;
    require(sky_model::effectiveSun(scene, options, 1) == scene, "Without the override the scene's sun is kept");
    options.sunOverride = true;
    options.sunElevation = 90;
    options.sunIntensity = 4;
    options.sunColor = {1, .5f, .25f};
    options.sunAngularDiameter = 1.5f;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const auto sun = sky_model::effectiveSun(scene, options, axis);
      require(std::abs(sun.direction[axis] + 1) < 1e-5f && sun.isDistant && sun.angularDiameterDegrees == 1.5f,
              "An overridden sun at the zenith shines straight down the world up axis");
      require(sun.radiance[0] == 4 && sun.radiance[1] == 2 && sun.radiance[2] == 1,
              "Override radiance is intensity times colour");
    }
    options.sunElevation = 0;
    options.sunAzimuth = 90;
    const auto horizon = sky_model::effectiveSun(scene, options, 1);
    require(std::abs(horizon.direction[1]) < 1e-5f && std::abs(horizon.direction[2] + 1) < 1e-5f,
            "Azimuth turns the override about the up axis");
    options.sunIntensity = 0;
    require(sky_model::radianceScale(sky_model::effectiveSun(scene, options, 1), options) == 0,
            "The procedural sky darkens with its sun");
    options.sunIntensity = 9;
    options.skyIntensity = 2;
    const float doubled = sky_model::radianceScale(sky_model::effectiveSun(scene, options, 1), options);
    options.skyIntensity = 1;
    const float single = sky_model::radianceScale(sky_model::effectiveSun(scene, options, 1), options);
    require(single > 0 && std::abs(doubled - 2 * single) < 1e-5f, "Sky brightness scales the procedural sky linearly");
    // A high sun: a blue zenith above a brighter, paler horizon, dim ground below.
    const auto texels = sky_model::texels({.3f, .9f, .2f});
    require(texels.size() == size_t(sky_model::width) * sky_model::height * 4, "One RGBA texel per sky direction");
    auto texel = [&](uint32_t x, uint32_t y) {
      const auto *t = &texels[(size_t(y) * sky_model::width + x) * 4];
      return Vec3{half::decode(t[0]), half::decode(t[1]), half::decode(t[2])};
    };
    const uint32_t column = sky_model::width / 4;
    const auto zenith = texel(column, 0), horizonSky = texel(column, sky_model::height / 2 - 1),
               ground = texel(column, sky_model::height - 1);
    require(zenith[2] > zenith[1] && zenith[1] > zenith[0], "The zenith is blue");
    require(horizonSky[0] / horizonSky[2] > zenith[0] / zenith[2], "The horizon is paler than the zenith");
    require(ground[0] + ground[1] + ground[2] < horizonSky[0] + horizonSky[1] + horizonSky[2],
            "The ground below the horizon is darker than the sky");
  }
  {
    // Static casters without a host bake scene.
    NativeResources resources;
    const auto mesh = quad(resources, 901, material(resources, 900, 1, 1, 1));
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = 902;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    identity.rasterVisible = 1;
    auto info = instance(mesh);
    info.pNext = &identity;
    const auto retained = resources.createRetained(info);
    std::vector<scene::Draw> draws = resources.retained(retained)->records;
    require(!draws.empty() && draws[0].classification == REMIXAPI_INSTANCE_STATIC_EXT, "Fixture draw is static");
    const auto key = s::staticCasterKey(draws);
    require(key != 0 && s::staticCasterKey(draws) == key, "An unchanged static draw set keeps its identity");
    const auto implied = s::implicitStaticScene(draws, 1);
    require(implied->ready && implied->draws.size() == draws.size() && scene::matchesBakeDraw(*implied, 0, draws[0]),
            "The implied scene is ready and matches its own draws");
    scene::SphereLight light;
    light.id = 10;
    light.radius = .25f;
    light.radiance[0] = 1;
    s::Planner planner;
    planner.update({&light, 1}, {}, 1, .001f, 100, 1, implied);
    require(planner.captured(draws[0]), "Implied static casters are frozen like a host scene's");
    auto moved = draws;
    moved[0].model[12] += 1;
    require(s::staticCasterKey(moved) != key && !planner.captured(moved[0]),
            "A moved static draw changes the identity and is no longer frozen");
    auto dynamic = draws;
    dynamic[0].classification = REMIXAPI_INSTANCE_DYNAMIC_EXT;
    require(s::staticCasterKey(dynamic) == 0 && s::implicitStaticScene(dynamic, 1)->draws.empty() &&
                !planner.captured(dynamic[0]),
            "Dynamic instances never join the implied static scene");
  }
  return 0;
}
