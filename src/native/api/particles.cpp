// Particle emitters (remix_particles_ext.h): the process's registry
// (scene/particles.h), which each Present publishes to the renderer.
#include "api/state.h"
#include "scene/particles.h"
#include <remix/remix_particles_ext.h>
#include <cstring>
#include <string>

using namespace remix::api;

namespace {
template <typename F> remixapi_ErrorCode particleCall(F &&call) {
  std::lock_guard lock(g_apiMutex);
  if (!g_runtime.isInitialized())
    return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  if (g_runtime.hasFailed())
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  return call(remix::scene::particles());
}
} // namespace

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateParticleEmitterEXT(
    const remixapi_ParticleEmitterInfoEXT *info, remixapi_ParticleEmitterHandleEXT *out) {
  if (!info || !out)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return particleCall([&](auto &particles) { return particles.create(*info, *out); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateParticleEmitterEXT(
    remixapi_ParticleEmitterHandleEXT emitter, const remixapi_ParticleEmitterInfoEXT *info) {
  if (!info)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return particleCall([&](auto &particles) { return particles.update(emitter, *info); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetParticleEmitterTransformEXT(
    remixapi_ParticleEmitterHandleEXT emitter, const remixapi_Transform *transform) {
  if (!transform)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return particleCall([&](auto &particles) { return particles.setTransform(emitter, *transform); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetParticleEmitterEnabledEXT(
    remixapi_ParticleEmitterHandleEXT emitter, remixapi_Bool enabled) {
  if (enabled != 0 && enabled != 1)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return particleCall([&](auto &particles) { return particles.setEnabled(emitter, enabled != 0); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_BurstParticleEmitterEXT(remixapi_ParticleEmitterHandleEXT emitter,
                                                                        uint32_t count) {
  return particleCall([&](auto &particles) { return particles.burst(emitter, count); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ClearParticleEmitterEXT(remixapi_ParticleEmitterHandleEXT emitter) {
  return particleCall([&](auto &particles) { return particles.clear(emitter); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_DestroyParticleEmitterEXT(remixapi_ParticleEmitterHandleEXT emitter) {
  return particleCall([&](auto &particles) { return particles.destroy(emitter); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetParticleBudgetEXT(uint32_t maxParticles,
                                                                      uint32_t maxSpawnsPerFrame) {
  std::lock_guard lock(g_apiMutex);
  if (!g_runtime.isInitialized())
    return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  // Both or neither: the ranges are checked before either is set.
  if (maxParticles < 1024 || maxParticles > 262144 || maxSpawnsPerFrame < 64 || maxSpawnsPerFrame > 65536)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  const auto status =
      g_runtime.configureSetting("rtx.native.particles.budget", std::to_string(maxParticles).c_str());
  if (status != REMIXAPI_ERROR_CODE_SUCCESS)
    return status;
  return g_runtime.configureSetting("rtx.native.particles.spawnBudget", std::to_string(maxSpawnsPerFrame).c_str());
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetParticleStatisticsEXT(remixapi_ParticleStatisticsEXT *out) {
  if (!out || out->structSize < sizeof(remixapi_ParticleStatisticsEXT))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return particleCall([&](auto &particles) {
    const auto statistics = particles.statistics();
    std::memcpy(out, &statistics, sizeof(statistics));
    return REMIXAPI_ERROR_CODE_SUCCESS;
  });
}
