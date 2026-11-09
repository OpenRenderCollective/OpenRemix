#include "api/state.h"
#include <remix/remix_bake_ext.h>
#include <remix/remix_reflection_ext.h>

extern "C" REMIXAPI uint32_t REMIXAPI_CALL remixapi_GetReflectionBindingVersionEXT(void) {
#if REMIX_METAL
  return 0;
#else
  return REMIXAPI_REFLECTION_BINDING_VERSION_EXT;
#endif
}

extern "C" REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_SetBakeSceneEXT(const remixapi_BakeSceneInfoEXT *info) {
#if REMIX_METAL
  (void)info;
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#else
  return remix::api::resourceCall([&](auto &resources) { resources.setBakeScene(info); });
#endif
}

extern "C" REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetBakeStatisticsEXT(remixapi_BakeStatisticsEXT *out) {
  if (!out || out->structSize < sizeof(*out))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
#if REMIX_METAL
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#else
  try {
    std::lock_guard lock(remix::api::g_apiMutex);
    if (!remix::api::g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    *out = remix::api::g_runtime.bakingStatistics();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
#endif
}

extern "C" REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_SetReflectionVolumesEXT(const remixapi_ReflectionVolumesInfoEXT *info) {
#if REMIX_METAL
  (void)info;
  return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
#else
  return remix::api::resourceCall([&](auto &resources) { resources.setReflectionVolumes(info); });
#endif
}
