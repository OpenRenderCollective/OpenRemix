// Host passes (remixapi_HostPassEXT): the host's screen-space passes with its
// own shaders, replayed on the composed frame before the UI.
#include "api/state.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <exception>
#include <SDL3/SDL.h>

using namespace remix::api;

namespace {
template <typename F> remixapi_ErrorCode hostCall(F &&call) {
  std::lock_guard lock(g_apiMutex);
  if (!g_runtime.isInitialized())
    return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  if (g_runtime.hasFailed())
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  try {
    return call(g_runtime.hostPasses());
  } catch (const std::exception &e) {
    SDL_Log("OpenRemix host pass call failed: %s", e.what());
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  } catch (...) {
    SDL_Log("OpenRemix host pass call failed");
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
} // namespace

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterHostShaderEXT(const remixapi_HostShaderInfoEXT *info,
                                                                        remixapi_HostShaderHandleEXT *out) {
  if (!info || !out || info->sType != REMIXAPI_STRUCT_TYPE_HOST_SHADER_INFO_EXT)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return hostCall([&](auto &passes) { return passes.registerShader(*info, *out); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyHostShaderEXT(remixapi_HostShaderHandleEXT shader) {
  return hostCall([&](auto &passes) { return passes.destroyShader(shader); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SubmitHostPassesEXT(const remixapi_HostPassListEXT *list) {
  if (!list || list->sType != REMIXAPI_STRUCT_TYPE_HOST_PASS_LIST_EXT)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return hostCall([&](auto &passes) { return passes.submit(*list); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateHostTargetEXT(remixapi_HostTargetHandleEXT target,
                                                                      uint32_t width, uint32_t height,
                                                                      uint32_t vkFormat, const void *pixels,
                                                                      uint64_t bytes) {
  if (!target || !width || !height || !pixels || bytes != uint64_t(width) * height * 4)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return hostCall([&](auto &passes) { return passes.updateTarget(target, width, height, vkFormat, pixels, bytes); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateHostVolumeEXT(remixapi_HostTargetHandleEXT target,
                                                                      uint32_t width, uint32_t height, uint32_t depth,
                                                                      uint32_t vkFormat, const void *pixels,
                                                                      uint64_t bytes) {
  if (!target || !width || !height || !depth || !pixels || bytes != uint64_t(width) * height * depth * 4)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return hostCall(
      [&](auto &passes) { return passes.updateTarget(target, width, height, vkFormat, pixels, bytes, depth); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetHostPassesEnabledEXT(remixapi_Bool *enabled) {
  if (!enabled)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return hostCall([&](auto &passes) {
    *enabled = passes.enabled();
    return REMIXAPI_ERROR_CODE_SUCCESS;
  });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetHostPassStatisticsEXT(remixapi_HostPassStatisticsEXT *out) {
  // Versioned by structSize: the first version ended before `always`.
  if (!out || out->structSize < offsetof(remixapi_HostPassStatisticsEXT, always))
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return hostCall([&](auto &passes) {
    auto statistics = passes.statistics();
    const size_t size = std::min(size_t(out->structSize), sizeof(statistics));
    statistics.structSize = size;
    std::memcpy(out, &statistics, size);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  });
}
