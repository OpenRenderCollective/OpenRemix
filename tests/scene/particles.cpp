#include "scene/particles.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
bool approx(float a, float b, float tolerance = 1e-4f) { return std::abs(a - b) <= tolerance; }

remixapi_ParticleEmitterInfoEXT plain() {
  remixapi_ParticleEmitterInfoEXT info{};
  info.sType = REMIXAPI_STRUCT_TYPE_PARTICLE_EMITTER_INFO_EXT;
  info.shape = REMIXAPI_PARTICLE_SHAPE_BOX_EXT;
  info.maxParticles = 1000;
  info.transform = {{{1, 0, 0, 5}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
  info.extent = {1, 1, 1};
  info.spawnRate = 100;
  info.lifetimeMin = info.lifetimeMax = 2;
  info.sizeStart = info.sizeEnd = .1f;
  info.colorStart = info.colorEnd = {1, 1, 1, 1};
  return info;
}
} // namespace

int main() {
  using namespace remix::scene;
  {
    // Validation.
    auto info = plain();
    require(!validateParticleEmitter(info), "A plain emitter was rejected");
    auto bad = info;
    bad.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    require(validateParticleEmitter(bad), "A wrong sType was accepted");
    bad = info;
    bad.shape = 4;
    require(validateParticleEmitter(bad), "An unknown shape was accepted");
    bad = info;
    bad.flags = 0x10;
    require(validateParticleEmitter(bad), "An unknown flag was accepted");
    bad = info;
    bad.maxParticles = maxParticlesPerEmitter + 1;
    require(validateParticleEmitter(bad), "Too many particles were accepted");
    bad = info;
    bad.lifetimeMin = 3;
    require(validateParticleEmitter(bad), "A lifetime minimum above its maximum was accepted");
    bad = info;
    bad.lifetimeMin = bad.lifetimeMax = 0;
    require(validateParticleEmitter(bad), "A zero lifetime was accepted");
    bad = info;
    bad.velocity.y = std::numeric_limits<float>::quiet_NaN();
    require(validateParticleEmitter(bad), "A NaN velocity was accepted");
    bad = info;
    bad.transform.matrix[1][3] = std::numeric_limits<float>::infinity();
    require(validateParticleEmitter(bad), "An infinite transform was accepted");
    bad = info;
    bad.coneAngleDegrees = 190;
    require(validateParticleEmitter(bad), "A cone above 180 degrees was accepted");
    bad = info;
    bad.sizeJitter = 2;
    require(validateParticleEmitter(bad), "A jitter above one was accepted");
    bad = info;
    bad.colorEnd.w = -1;
    require(validateParticleEmitter(bad), "A negative colour was accepted");
    // A preset ignores the fields it fills.
    bad = info;
    bad.preset = REMIXAPI_PARTICLE_PRESET_RAIN_EXT;
    bad.lifetimeMin = bad.lifetimeMax = 0;
    require(!validateParticleEmitter(bad), "A preset's own fields were validated");
    bad.spawnRate = -1;
    require(validateParticleEmitter(bad), "A negative spawn rate was accepted with a preset");
  }
  {
    // Presets: camera volumes in world axes, scaled to the scene's units.
    remixapi_ParticleEmitterInfoEXT info{};
    info.sType = REMIXAPI_STRUCT_TYPE_PARTICLE_EMITTER_INFO_EXT;
    info.preset = REMIXAPI_PARTICLE_PRESET_RAIN_EXT;
    // Y up, a centimetre a unit.
    const auto rain = resolveParticleEmitter(info, .01f, 1);
    require(rain.shape == REMIXAPI_PARTICLE_SHAPE_CAMERA_VOLUME_EXT, "Rain was not a camera volume");
    require(rain.facing == REMIXAPI_PARTICLE_FACING_VELOCITY_EXT, "Rain was not stretched along its motion");
    require(approx(rain.velocity[1], -900) && approx(rain.velocity[0], 0) && approx(rain.velocity[2], 0),
            "Rain did not fall along the up axis in scene units");
    require(approx(rain.axis[1], 1), "The volume's +Z was not the up axis");
    require(approx(rain.extent[2], 800) && approx(rain.extent[0], 1400), "Rain's volume was not scaled");
    // Emitter z (up) is world y: the transform's column 2 is +Y.
    require(approx(rain.transform[1][2], 1) && approx(rain.transform[2][0], 1) && approx(rain.transform[0][1], 1),
            "The up basis was not a cyclic permutation");
    info.preset = REMIXAPI_PARTICLE_PRESET_SNOW_EXT;
    info.extent = {1, 2, 3};
    const auto snow = resolveParticleEmitter(info, 1, 2);
    require(snow.facing == REMIXAPI_PARTICLE_FACING_CAMERA_EXT, "Snow was stretched");
    require(approx(snow.extent[0], 1) && approx(snow.extent[2], 3), "A preset replaced a given extent");
    require(snow.acceleration[2] < 0 && snow.drag > 0 && snow.turbulence > 0, "Snow did not drift down");
  }
  {
    // Plain emitters: velocity and cone axis rotate with the transform.
    auto info = plain();
    info.transform = {{{0, -1, 0, 5}, {1, 0, 0, 0}, {0, 0, 1, 0}}}; // 90 degrees about Z
    info.velocity = {2, 0, 0};
    info.coneAngleDegrees = 90;
    const auto resolved = resolveParticleEmitter(info, 1, 2);
    require(approx(resolved.velocity[1], 2) && approx(resolved.velocity[0], 0), "Velocity did not rotate");
    require(approx(resolved.cosCone, 0), "The cone's cosine was wrong");
    require(approx(resolved.transform[0][3], 5), "The transform's translation was lost");
  }
  {
    RangeAllocator pool;
    pool.reset(100);
    const uint32_t a = pool.allocate(40), b = pool.allocate(40);
    require(a == 0 && b == 40, "Ranges were not first fit");
    require(pool.allocate(30) == UINT32_MAX, "A range larger than the free space was given");
    pool.release(a, 40);
    require(pool.allocate(30) == 0, "A released range was not reused");
    pool.release(0, 30);
    pool.release(b, 40);
    require(pool.used() == 0 && pool.allocate(100) == 0, "Released ranges were not coalesced");
  }
  {
    ParticleRegistry registry;
    registry.setRendered(true);
    auto info = plain();
    remixapi_ParticleEmitterHandleEXT handle = 0, other = 0;
    require(registry.create(info, handle) == REMIXAPI_ERROR_CODE_SUCCESS && handle, "Create failed");
    auto bad = info;
    bad.shape = 9;
    require(registry.create(bad, other) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "An invalid create succeeded");
    require(registry.update(handle + 100, info) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "An unknown handle was updated");
    ParticleRegistry::PublishSettings settings;
    settings.seconds = .1;
    settings.budget = 1500;
    auto frame = registry.publish(settings);
    require(frame.enabled && frame.emitters.size() == 1, "The emitter was not published");
    require(frame.emitters[0].capacity == 1000 && frame.emitters[0].offset == 0, "The slice was wrong");
    require(frame.emitters[0].reset, "A new emitter was not reset");
    require(frame.emitters[0].spawn == 10, "100 per second for 0.1 s did not spawn 10");
    frame = registry.publish(settings);
    require(!frame.emitters[0].reset, "An emitter was reset every frame");
    // Fractional spawns carry over.
    settings.seconds = .0625;
    uint32_t spawned = 0;
    for (int i = 0; i < 20; ++i)
      spawned += registry.publish(settings).emitters[0].spawn;
    require(spawned == 125, "Fractional spawns were lost");
    // A second emitter does not fit the budget's remainder until the first goes.
    require(registry.create(info, other) == REMIXAPI_ERROR_CODE_SUCCESS, "A second create failed");
    frame = registry.publish(settings);
    require(frame.emitters.size() == 1 && frame.emitters[0].handle == handle, "An emitter past the budget was published");
    require(registry.statistics().allocatedParticles == 1000, "Allocated particles were wrong");
    require(registry.destroy(handle) == REMIXAPI_ERROR_CODE_SUCCESS, "Destroy failed");
    require(registry.destroy(handle) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "A destroyed emitter was destroyed");
    frame = registry.publish(settings);
    require(frame.emitters.size() == 1 && frame.emitters[0].handle == other && frame.emitters[0].reset,
            "The waiting emitter did not take the freed slice");
    // Bursts, clears and disabling.
    require(registry.burst(other, 25) == REMIXAPI_ERROR_CODE_SUCCESS, "Burst failed");
    require(registry.setEnabled(other, false) == REMIXAPI_ERROR_CODE_SUCCESS, "Disable failed");
    // Frames longer than a quarter of a second count as a quarter.
    settings.seconds = 1;
    frame = registry.publish(settings);
    require(frame.emitters[0].spawn == 25, "A disabled emitter's burst was not spawned alone");
    require(registry.clear(other) == REMIXAPI_ERROR_CODE_SUCCESS, "Clear failed");
    frame = registry.publish(settings);
    require(frame.emitters[0].reset && frame.emitters[0].spawn == 0, "Clear did not reset the emitter");
    // The spawn budget is shared in proportion.
    registry.setEnabled(other, true);
    info.spawnRate = 4000;
    registry.update(other, info);
    settings.spawnBudget = 100;
    frame = registry.publish(settings);
    require(frame.emitters[0].spawn == 100 && frame.spawns == 100, "The spawn budget was exceeded");
    require(registry.statistics().spawnsDropped == 900, "Dropped spawns were not counted");
    // A new size takes a new slice and clears.
    info.maxParticles = 500;
    registry.update(other, info);
    frame = registry.publish(settings);
    require(frame.emitters[0].capacity == 500 && frame.emitters[0].reset, "A resized emitter kept its slice");
    // FOLLOW_WEATHER scales the rate.
    info.flags = REMIXAPI_PARTICLE_FOLLOW_WEATHER_EXT;
    info.spawnRate = 400;
    registry.update(other, info);
    settings.spawnBudget = 8192;
    settings.precipitation = .25f;
    frame = registry.publish(settings);
    require(frame.emitters[0].spawn == 25, "FOLLOW_WEATHER did not scale the rate");
    // Disabled particles start over.
    settings.enabled = false;
    frame = registry.publish(settings);
    require(!frame.enabled && frame.emitters.empty(), "A disabled system published emitters");
    settings.enabled = true;
    frame = registry.publish(settings);
    require(frame.resetAll && frame.emitters[0].reset, "Particles enabled again did not start empty");
    // The UI's test emitters come and go with the settings and the API
    // cannot destroy them.
    settings.rain = true;
    frame = registry.publish(settings);
    require(frame.emitters.size() == 1 && registry.statistics().emitters == 2,
            "The rain test emitter was not created (or fitted)");
    settings.budget = 65536;
    frame = registry.publish(settings);
    require(frame.resetAll && frame.emitters.size() == 2, "A new budget did not reallocate");
    remixapi_ParticleEmitterHandleEXT rain = 0;
    for (const auto &emitter : frame.emitters)
      if (emitter.handle != other)
        rain = emitter.handle;
    require(registry.destroy(rain) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "The API destroyed a test emitter");
    settings.rain = false;
    frame = registry.publish(settings);
    require(frame.emitters.size() == 1 && registry.statistics().emitters == 1, "The rain test emitter stayed");
    registry.report({.live = 123, .dropped = 4, .sheltered = 5});
    const auto statistics = registry.statistics();
    require(statistics.liveParticles == 123 && statistics.spawnsSheltered == 5 &&
                statistics.structSize == sizeof(statistics) && statistics.enabled,
            "Statistics were wrong");
    registry.reset();
    require(registry.statistics().emitters == 0, "Reset kept emitters");
    // The emitter limit.
    for (uint32_t i = 0; i < maxParticleEmitters; ++i)
      require(registry.create(info, other) == REMIXAPI_ERROR_CODE_SUCCESS, "Create below the limit failed");
    require(registry.create(info, other) == REMIXAPI_ERROR_CODE_GENERAL_FAILURE, "The emitter limit was exceeded");
  }
  std::puts("Particle registry checks passed");
  return 0;
}
