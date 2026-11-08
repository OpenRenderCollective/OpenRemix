/*
 * Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "api/state.h"
#include "api/entrypoints.h"

using namespace remix::api;

namespace {
  bool isVersionCompatible(uint64_t userVersion) {
    constexpr uint64_t kCompiledVersion =
        REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
    const bool isDevelopment =
        REMIXAPI_VERSION_GET_MAJOR(userVersion) == 0 && REMIXAPI_VERSION_GET_MAJOR(kCompiledVersion) == 0;
    if (isDevelopment) {
      return REMIXAPI_VERSION_GET_MINOR(userVersion) == REMIXAPI_VERSION_GET_MINOR(kCompiledVersion);
    }
    return REMIXAPI_VERSION_GET_MAJOR(userVersion) == REMIXAPI_VERSION_GET_MAJOR(kCompiledVersion) &&
           REMIXAPI_VERSION_GET_MINOR(userVersion) <= REMIXAPI_VERSION_GET_MINOR(kCompiledVersion);
  }

} // namespace

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_InitializeLibrary(const remixapi_InitializeLibraryInfo *info,
                                                                     remixapi_Interface *outResult) {
  if (outResult) {
    *outResult = {};
  }
  if (info == nullptr || outResult == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  if (!isVersionCompatible(info->version)) {
    return REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION;
  }

  remixapi_Interface result = {};
  result.Shutdown = remixapi_Shutdown;
  result.CreateMaterial = createMaterial;
  result.DestroyMaterial = destroyMaterial;
  result.CreateMesh = createMesh;
  result.CreateMeshBatched = createMesh;
  result.DestroyMesh = destroyMesh;
  result.SetupCamera = setupCamera;
  result.DrawInstance = drawInstance;
  result.CreateLight = createLight;
  result.CreateLightBatched = createLight;
  result.DestroyLight = destroyLight;
  result.DrawLightInstance = drawLightInstance;
  result.SetConfigVariable = setConfigVariable;
  result.AddTextureHash = addTextureHash;
  result.RemoveTextureHash = removeTextureHash;
  result.CreateTexture = createTexture;
  result.DestroyTexture = destroyTexture;
  result.pick_RequestObjectPicking = requestObjectPicking;
  result.pick_HighlightObjects = highlightObjects;
  result.Startup = remixapi_Startup;
  result.Present = remixapi_Present;
  result.SetCameraMediumMaterial = setCameraMediumMaterial;
  result.GetUIState = remixapi_GetUIState;
  result.SetUIState = remixapi_SetUIState;
  result.RegisterCallbacks = remixapi_RegisterCallbacks;
  result.AutoInstancePersistentLights = remixapi_AutoInstancePersistentLights;
  result.UpdateLightDefinition = remixapi_UpdateLightDefinition;
  result.DrawScreenOverlay = drawScreenOverlay;
  result.SetGameValue = remixapi_SetGameValue;
  result.RequestVramCompaction = requestVramCompaction;
  result.GetVramStats = getVramStats;
  result.RequestTextureVramFree = requestTextureVramFree;
  result.GetGameValue = remixapi_GetGameValue;
  result.SetFogState = remixapi_SetFogState;
  result.SetScreenTint = remixapi_SetScreenTint;
  result.RegisterUITexture = remixapi_RegisterUITexture;
  result.FreeUITexture = remixapi_FreeUITexture;
  result.SubmitUIDrawList = remixapi_SubmitUIDrawList;
  result.RequestPresentedScreenshot = remixapi_RequestPresentedScreenshot;
  result.PumpEvents = remixapi_PumpEvents;
  result.GetWindowState = remixapi_GetWindowState;
  result.PollMouseState = remixapi_PollMouseState;
  result.IsKeyDown = remixapi_IsKeyDown;
  result.SetMouseGrabbed = remixapi_SetMouseGrabbed;
  result.SetCursorPosition = remixapi_SetCursorPosition;
  result.SetFullscreen = remixapi_SetFullscreen;
  result.GetFeatureAvailability = remixapi_GetFeatureAvailability;

#if defined(_WIN32)
  result.dxvk_CreateD3D9 = createD3D9;
  result.dxvk_RegisterD3D9Device = registerD3D9;
  result.dxvk_GetExternalSwapchain = getExternalSwapchain;
  result.dxvk_GetVkImage = getVkImage;
  result.dxvk_CopyRenderingOutput = copyOutput;
  result.dxvk_SetDefaultOutput = defaultOutput;
  result.dxvk_GetTextureHash = getTextureHash;
  result.dxvk_GetSharedD3D11TextureHandle = getSharedTexture;
  static_assert(sizeof(result) == 440);
#else
  static_assert(sizeof(result) == 376);
#endif
  *outResult = result;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
}
