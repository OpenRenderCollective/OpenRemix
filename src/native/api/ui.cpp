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
#include <remix/remix_native_ext.h>

using namespace remix::api;

extern "C" {

REMIXAPI remixapi_UIState REMIXAPI_CALL remixapi_GetUIState() {
  try {
    std::lock_guard lock(g_apiMutex);
    return g_runtime.uiState();
  } catch (...) {
    return REMIXAPI_UI_STATE_NONE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetUIState(remixapi_UIState state) {
  if (state < REMIXAPI_UI_STATE_NONE || state > REMIXAPI_UI_STATE_ADVANCED) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  try {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized()) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    g_runtime.setUiState(state);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetScreenTint(float colorR, float colorG, float colorB,
                                                                 float alpha) {
  return resourceCall([&](auto &raster) { raster.setTint(colorR, colorG, colorB, alpha); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterUITexture(remixapi_UITextureHandle id, uint32_t width,
                                                                     uint32_t height, remixapi_Format format,
                                                                     const void *pixelData, uint64_t dataSize) {
  if (!id || !width || !height || !pixelData || uint64_t(width) * height > UINT64_MAX / 4 ||
      dataSize != uint64_t(width) * height * 4 ||
      (format != REMIXAPI_FORMAT_R8G8B8A8_UNORM && format != REMIXAPI_FORMAT_R8G8B8A8_SRGB &&
       format != REMIXAPI_FORMAT_B8G8R8A8_UNORM && format != REMIXAPI_FORMAT_B8G8R8A8_SRGB)) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  return resourceCall([&](auto &raster) { raster.registerUiTexture(id, width, height, format, pixelData, dataSize); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateUITextureRegionEXT(remixapi_UITextureHandle id, uint32_t x,
                                                                            uint32_t y, uint32_t width, uint32_t height,
                                                                            remixapi_Format format, const void *pixels,
                                                                            uint32_t rowPitch) {
  if (!id || !width || !height || !pixels)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return resourceCall(
      [&](auto &raster) { raster.updateUiTextureRegion(id, x, y, width, height, format, pixels, rowPitch); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_FreeUITexture(remixapi_UITextureHandle id) {
  if (!id) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  return resourceCall([&](auto &raster) { raster.freeUiTexture(id); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SubmitUIDrawList(const remixapi_UIDrawList *drawList) {
  if (drawList && drawList->sType != REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  return resourceCall([&](auto &raster) { raster.submitUi(drawList); });
}
}
