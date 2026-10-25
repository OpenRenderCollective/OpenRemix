#include <remix/remix_particles_ext.h>
#include <cstdio>
#include <cstdlib>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
} // namespace

// The particle exports before Startup: arguments are checked first, then
// every call reports that the runtime is not initialized.
int main() {
  remixapi_ParticleEmitterInfoEXT info{};
  info.sType = REMIXAPI_STRUCT_TYPE_PARTICLE_EMITTER_INFO_EXT;
  info.preset = REMIXAPI_PARTICLE_PRESET_RAIN_EXT;
  remixapi_ParticleEmitterHandleEXT handle = 0;
  require(remixapi_CreateParticleEmitterEXT(nullptr, &handle) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "A null emitter info was accepted");
  require(remixapi_CreateParticleEmitterEXT(&info, nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "A null handle output was accepted");
  require(remixapi_CreateParticleEmitterEXT(&info, &handle) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED,
          "An emitter was created before startup");
  require(remixapi_UpdateParticleEmitterEXT(1, nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "A null update was accepted");
  require(remixapi_SetParticleEmitterTransformEXT(1, nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "A null transform was accepted");
  require(remixapi_SetParticleEmitterEnabledEXT(1, 2) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "A boolean other than 0/1 was accepted");
  require(remixapi_BurstParticleEmitterEXT(1, 10) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED, "Burst before startup");
  require(remixapi_ClearParticleEmitterEXT(1) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED, "Clear before startup");
  require(remixapi_DestroyParticleEmitterEXT(1) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED, "Destroy before startup");
  require(remixapi_SetParticleBudgetEXT(65536, 8192) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED,
          "Budget before startup");
  remixapi_ParticleStatisticsEXT statistics{};
  require(remixapi_GetParticleStatisticsEXT(&statistics) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Statistics without a structure size were accepted");
  statistics.structSize = sizeof(statistics);
  require(remixapi_GetParticleStatisticsEXT(&statistics) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED,
          "Statistics before startup");
  std::puts("Particle API argument checks passed");
  return 0;
}
