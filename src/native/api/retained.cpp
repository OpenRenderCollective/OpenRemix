// Scene instances (remix_native_ext.h): instances the runtime keeps and draws
// with one call a frame. Named exports only; the versioned interface table is
// unchanged. Validation, resolution and the per-frame draw live in
// scene/retained.cpp. Every call takes the API mutex like the other entry
// points.

#include "api/state.h"
#if REMIX_HAS_USD
#include "usd/capture.h"
#endif

using namespace remix::api;

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_AddSceneInstancesEXT(const remixapi_InstanceInfo *infos,
                                                                        uint32_t count, uint64_t *outInstances) {
  if (count && (!infos || !outInstances))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return resourceCall([&](auto &resources) {
    for (uint32_t i = 0; i < count; ++i)
      if (infos[i].sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO)
        throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    uint32_t added = 0;
    try {
      for (; added < count; ++added)
        outInstances[added] = resources.createRetained(infos[added]);
    } catch (...) {
      // All or nothing: a refused description takes the ones before it along.
      resources.destroyRetained(outInstances, added);
      std::fill_n(outInstances, count, uint64_t(0));
      throw;
    }
  });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSceneInstanceEXT(uint64_t instance,
                                                                       const remixapi_InstanceInfo *info) {
  return resourceCall([&](auto &resources) {
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO)
      throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    resources.updateRetained(instance, *info);
  });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_ChangeSceneInstancesEXT(const remixapi_SceneInstanceChangeEXT *changes, uint32_t count) {
  return resourceCall([&](auto &resources) { resources.updateRetained(changes, count); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RemoveSceneInstancesEXT(const uint64_t *instances,
                                                                           uint32_t count) {
  return resourceCall([&](auto &resources) { resources.destroyRetained(instances, count); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawSceneInstancesEXT(const float *origin) {
  return resourceCall([&](auto &resources) {
    resources.drawRetained(origin);
#if REMIX_HAS_USD
    remix::usd::captureSceneInstances(resources, origin);
#endif
  });
}
}
