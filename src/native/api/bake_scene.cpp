#include "api/state.h"
#include <algorithm>
#include <cstring>
#include <remix/remix_bake_ext.h>

extern "C" REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_SetBakeSceneEXT(const remixapi_BakeSceneInfoEXT *info) {
  return remix::api::resourceCall([&](auto &resources) { resources.setBakeScene(info); });
}

extern "C" REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetBakeStatisticsEXT(remixapi_BakeStatisticsEXT *out) {
  if (!out || out->structSize < 8)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  try {
    std::lock_guard lock(remix::api::g_apiMutex);
    if (!remix::api::g_runtime.isInitialized())
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    const auto statistics = remix::api::g_runtime.bakingStatistics();
    std::memcpy(out, &statistics, std::min(size_t(out->structSize), sizeof(statistics)));
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

extern "C" REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_SetReflectionVolumesEXT(const remixapi_ReflectionVolumesInfoEXT *info) {
  return remix::api::resourceCall([&](auto &resources) { resources.setReflectionVolumes(info); });
}
