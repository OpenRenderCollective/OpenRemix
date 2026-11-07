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

#pragma once
#include <remix/remix_c.h>

namespace remix::api {
  remixapi_ErrorCode REMIXAPI_CALL createMaterial(const remixapi_MaterialInfo *info,
                                                  remixapi_MaterialHandle *outHandle);
  remixapi_ErrorCode REMIXAPI_CALL destroyMaterial(remixapi_MaterialHandle handle);
  remixapi_ErrorCode REMIXAPI_CALL createMesh(const remixapi_MeshInfo *info, remixapi_MeshHandle *outHandle);
  remixapi_ErrorCode REMIXAPI_CALL destroyMesh(remixapi_MeshHandle handle);
  remixapi_ErrorCode REMIXAPI_CALL setupCamera(const remixapi_CameraInfo *info);
  remixapi_ErrorCode REMIXAPI_CALL setCameraMediumMaterial(const remixapi_CameraMediumInfo *info);
  remixapi_ErrorCode REMIXAPI_CALL drawInstance(const remixapi_InstanceInfo *info);
  remixapi_ErrorCode REMIXAPI_CALL createLight(const remixapi_LightInfo *info, remixapi_LightHandle *outHandle);
  remixapi_ErrorCode REMIXAPI_CALL destroyLight(remixapi_LightHandle handle);
  remixapi_ErrorCode REMIXAPI_CALL drawLightInstance(remixapi_LightHandle handle);
  remixapi_ErrorCode REMIXAPI_CALL setConfigVariable(const char *key, const char *value);
  remixapi_ErrorCode REMIXAPI_CALL addTextureHash(const char *category, const char *hash);
  remixapi_ErrorCode REMIXAPI_CALL removeTextureHash(const char *category, const char *hash);
  remixapi_ErrorCode REMIXAPI_CALL createTexture(const remixapi_TextureInfo *info, remixapi_TextureHandle *outHandle);
  remixapi_ErrorCode REMIXAPI_CALL destroyTexture(remixapi_TextureHandle handle);
  remixapi_ErrorCode REMIXAPI_CALL requestObjectPicking(const remixapi_Rect2D *pixelRegion,
                                                        PFN_remixapi_pick_RequestObjectPickingUserCallback callback,
                                                        void *);
  remixapi_ErrorCode REMIXAPI_CALL highlightObjects(const uint32_t *values, uint32_t valueCount, uint8_t, uint8_t,
                                                    uint8_t);
  remixapi_ErrorCode REMIXAPI_CALL drawScreenOverlay(const void *pixels, uint32_t width, uint32_t height,
                                                     remixapi_Format format, float opacity);
  remixapi_ErrorCode REMIXAPI_CALL requestTextureVramFree();
  remixapi_ErrorCode REMIXAPI_CALL requestVramCompaction();
  remixapi_ErrorCode REMIXAPI_CALL getVramStats(remixapi_VramStats *stats);
#if defined(_WIN32)
  remixapi_ErrorCode REMIXAPI_CALL createD3D9(remixapi_Bool, IDirect3D9Ex **output);
  remixapi_ErrorCode REMIXAPI_CALL registerD3D9(IDirect3DDevice9Ex *device);
  remixapi_ErrorCode REMIXAPI_CALL getExternalSwapchain(uint64_t *image, uint64_t *done, uint64_t *resume);
  remixapi_ErrorCode REMIXAPI_CALL getVkImage(IDirect3DSurface9 *source, uint64_t *output);
  remixapi_ErrorCode REMIXAPI_CALL copyOutput(IDirect3DSurface9 *target, remixapi_dxvk_CopyRenderingOutputType type);
  remixapi_ErrorCode REMIXAPI_CALL defaultOutput(remixapi_dxvk_CopyRenderingOutputType type,
                                                 const remixapi_Float4D *color);
  remixapi_ErrorCode REMIXAPI_CALL getTextureHash(IDirect3DTexture9 *texture, uint64_t *output);
  remixapi_ErrorCode REMIXAPI_CALL getSharedTexture(void **handle, uint32_t *width, uint32_t *height);
#endif
} // namespace remix::api
