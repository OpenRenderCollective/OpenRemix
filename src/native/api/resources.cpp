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
#include "scene/types.h"
#include <remix/remix_native_ext.h>
#if REMIX_HAS_USD
#include "usd/capture.h"
#include "usd/mods.h"
#endif
#include "api/entrypoints.h"
#include "scene/images.h"
#include <remix/remix_native_ext.h>
#include <algorithm>
#include <optional>

namespace remix::api {
  remixapi_ErrorCode REMIXAPI_CALL createMaterial(const remixapi_MaterialInfo *info,
                                                  remixapi_MaterialHandle *outHandle) {
    if (outHandle) {
      *outHandle = nullptr;
    }
    if (!info || !outHandle || info->sType != REMIXAPI_STRUCT_TYPE_MATERIAL_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return resourceCall([&](auto &raster) { raster.createMaterial(*info, *outHandle); });
  }

  remixapi_ErrorCode REMIXAPI_CALL destroyMaterial(remixapi_MaterialHandle handle) {
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return resourceCall([&](auto &raster) { raster.destroyMaterial(handle); });
  }

  remixapi_ErrorCode REMIXAPI_CALL createMesh(const remixapi_MeshInfo *info, remixapi_MeshHandle *outHandle) {
    if (outHandle) {
      *outHandle = nullptr;
    }
    if (!info || !outHandle || info->sType != REMIXAPI_STRUCT_TYPE_MESH_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return resourceCall([&](auto &raster) { raster.createMesh(*info, *outHandle); });
  }

  remixapi_ErrorCode REMIXAPI_CALL destroyMesh(remixapi_MeshHandle handle) {
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return resourceCall([&](auto &raster) { raster.destroyMesh(handle); });
  }

  remixapi_ErrorCode REMIXAPI_CALL setupCamera(const remixapi_CameraInfo *info) {
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_CAMERA_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return resourceCall([&](auto &raster) { raster.setupCamera(*info); });
  }

  remixapi_ErrorCode REMIXAPI_CALL drawInstance(const remixapi_InstanceInfo *info) {
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return resourceCall([&](auto &raster) {
#if REMIX_HAS_USD
      // Recorded before mods replace it: captures keep the game's own hashes.
      remix::usd::captureInstance(*info);
      const auto *replacement = static_cast<const remixapi_InstanceInfoReplacementHashEXT *>(
          remix::scene::extension(info->pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_REPLACEMENT_HASH_EXT));
      // A halo source outlines the submitted mesh: a mod's replacement (and its
      // lights) is drawn once, by the scene instance.
      // Mask sources and masked overlays likewise draw what was submitted; a
      // CUT instance is a scene instance, replaced like any other.
      const auto *mask = static_cast<const remixapi_InstanceInfoMaskEXT *>(
          remix::scene::extension(info->pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MASK_EXT));
      const bool halo =
          remix::scene::extension(info->pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_HALO_EXT) != nullptr ||
          (mask && mask->role != REMIXAPI_INSTANCE_MASK_CUT_EXT);
      if (!halo && remix::usd::modInstance(raster, *info, replacement ? replacement->meshHash : 0))
        return;
#endif
      raster.drawInstance(*info);
    });
  }

  remixapi_ErrorCode REMIXAPI_CALL createLight(const remixapi_LightInfo *info,
                                               remixapi_LightHandle *outHandle) {
    if (outHandle) {
      *outHandle = nullptr;
    }
    if (!info || !outHandle || info->sType != REMIXAPI_STRUCT_TYPE_LIGHT_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return resourceCall([&](auto &resources) { resources.createLight(*info, *outHandle); });
  }

  remixapi_ErrorCode REMIXAPI_CALL destroyLight(remixapi_LightHandle handle) {
    if (!handle)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    return resourceCall([&](auto &resources) { resources.destroyLight(handle); });
  }

  remixapi_ErrorCode REMIXAPI_CALL drawLightInstance(remixapi_LightHandle handle) {
    if (!handle)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    return resourceCall([&](auto &resources) {
#if REMIX_HAS_USD
      remix::usd::captureLight(handle);
      if (remix::usd::modLight(resources, handle))
        return;
#endif
      resources.drawLight(handle);
    });
  }

  remixapi_ErrorCode REMIXAPI_CALL createTexture(const remixapi_TextureInfo *info,
                                                 remixapi_TextureHandle *outHandle) {
    if (outHandle) {
      *outHandle = nullptr;
    }
    if (!info || !outHandle || info->sType != REMIXAPI_STRUCT_TYPE_TEXTURE_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    // "Keep BC textures compressed": copying, hashing and scanning the blocks
    // is a pure function of the caller's data, so it runs before the API
    // mutex and never stalls a Present. Errors surface after the usual
    // initialization checks, as they would inside the lock.
    std::optional<scene::Pixels> prepared;
    auto prepareError = REMIXAPI_ERROR_CODE_SUCCESS;
    const auto *mips = static_cast<const remixapi_TextureInfoGenerateMipsEXT *>(
        scene::extension(info->pNext, REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT));
    // (A dynamic texture is never compressed: the resource table rejects it.)
    if (info->hash && g_runtime.raster().keepCompressedTextures() && scene::isBlockCompressed(info->format) &&
        !(mips && mips->generate && info->mipLevels <= 1) &&
        !scene::extension(info->pNext, REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT)) {
      try {
        prepared = scene::compressTexture(*info);
      } catch (remixapi_ErrorCode code) {
        prepareError = code;
      } catch (...) {
        prepareError = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
    }
    return resourceCall([&](auto &raster) {
      if (prepareError != REMIXAPI_ERROR_CODE_SUCCESS)
        throw prepareError;
      if (prepared)
        raster.createTexture(*info, std::move(*prepared), *outHandle);
      else
        raster.createTexture(*info, *outHandle);
    });
  }

  remixapi_ErrorCode REMIXAPI_CALL destroyTexture(remixapi_TextureHandle handle) {
    if (!handle) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return resourceCall([&](auto &raster) { raster.destroyTexture(handle); });
  }

  remixapi_ErrorCode REMIXAPI_CALL drawScreenOverlay(const void *pixels, uint32_t width, uint32_t height,
                                                     remixapi_Format format, float opacity) {
    return resourceCall([&](auto &raster) { raster.overlay(pixels, width, height, format, opacity); });
  }

} // namespace remix::api

using namespace remix::api;

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateLightDefinition(remixapi_LightHandle handle,
                                                                         const remixapi_LightInfo *info) {
  if (!handle || !info || info->sType != REMIXAPI_STRUCT_TYPE_LIGHT_INFO) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  return resourceCall([&](auto &resources) { resources.updateLight(handle, *info); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateTextureEXT(remixapi_TextureHandle handle, uint32_t x,
                                                                    uint32_t y, uint32_t width, uint32_t height,
                                                                    remixapi_Format format, const void *pixels,
                                                                    uint32_t rowPitch) {
  if (!handle || !width || !height || !pixels)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return resourceCall(
      [&](auto &resources) { resources.updateTexture(handle, x, y, width, height, format, pixels, rowPitch); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_SetVisibilityPartitionEXT(const remixapi_VisibilityPartitionEXT *info) {
  std::shared_ptr<remix::scene::VisibilityPartition> partition;
  if (info && info->nodeCount) {
    if (info->structSize != sizeof(*info) || !info->planes || !info->children || !info->leafClusters ||
        (info->clusterCount && !info->visibility) || info->rowBytes < (info->clusterCount + 7) / 8)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    static uint64_t version = 0;
    partition = std::make_shared<remix::scene::VisibilityPartition>();
    partition->nodes.resize(info->nodeCount);
    for (uint32_t i = 0; i < info->nodeCount; ++i) {
      auto &node = partition->nodes[i];
      std::copy_n(info->planes + size_t(i) * 4, 4, node.plane);
      node.children[0] = info->children[size_t(i) * 2];
      node.children[1] = info->children[size_t(i) * 2 + 1];
      node.reserved[0] = node.reserved[1] = 0;
    }
    partition->leafClusters.assign(info->leafClusters, info->leafClusters + info->leafCount);
    partition->clusterCount = info->clusterCount;
    partition->rowBytes = info->rowBytes;
    partition->visibility.assign(info->visibility, info->visibility + size_t(info->clusterCount) * info->rowBytes);
    partition->closed = true;
    for (const auto &node : partition->nodes)
      for (int32_t child : node.children)
        partition->closed &= child >= 0 ? uint32_t(child) < info->nodeCount
                                        : uint64_t(-1 - int64_t(child)) < info->leafCount;
    partition->version = ++version;
  } else if (info && info->structSize != sizeof(*info)) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  std::lock_guard lock(g_apiMutex);
  if (!g_runtime.isInitialized())
    return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  g_runtime.raster().setVisibilityPartition(std::move(partition));
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
}
