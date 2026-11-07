#pragma once
#include "scene/resources.h"
#include <remix/remix_native_ext.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace remix::test {
  inline void require(bool value, const char *what) {
    if (!value) {
      std::fprintf(stderr, "Raster test failed: %s\n", what);
      std::exit(1);
    }
  }
  template <typename F> void rejected(F &&call) {
    try {
      call();
    } catch (remixapi_ErrorCode) {
      return;
    }
    require(false, "Invalid input accepted");
  }

  inline remixapi_MaterialHandle material(remix::NativeResources &r, uint64_t hash, float red, float green, float blue,
                                          float alpha = 1, remixapi_Path texture = nullptr, int compare = 7) {
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {red, green, blue};
    opaque.opacityConstant = alpha;
    opaque.alphaTestType = compare;
    opaque.alphaReferenceValue = 128;
    remixapi_MaterialInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.hash = hash;
    info.pNext = &opaque;
    info.albedoTexture = texture;
    remixapi_MaterialHandle result;
    r.createMaterial(info, result);
    return result;
  }
  inline remixapi_MeshHandle quad(remix::NativeResources &r, uint64_t hash, remixapi_MaterialHandle mat,
                                  uint32_t color = 0xffffffff, bool skin = false) {
    remixapi_HardcodedVertex vertices[4] = {};
    const float positions[4][3] = {{-.8f, -.8f, .5f}, {.8f, -.8f, .5f}, {.8f, .8f, .5f}, {-.8f, .8f, .5f}};
    for (int i = 0; i < 4; ++i) {
      std::copy_n(positions[i], 3, vertices[i].position);
      vertices[i].color = color;
      vertices[i].texcoord[0] = i == 1 || i == 2 ? 1.f : 0.f;
      vertices[i].texcoord[1] = i >= 2 ? 0.f : 1.f;
    }
    // The camera looks along +Z; the front of this quad points toward -Z.
    const uint32_t indices[] = {0, 2, 1, 0, 3, 2}, bones[] = {0, 0, 0, 0};
    const float weights[] = {1, 1, 1, 1};
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = vertices;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = mat;
    if (skin) {
      surface.skinning_hasvalue = 1;
      surface.skinning_value = {1, weights, 4, bones, 4};
    }
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = hash;
    info.surfaces_values = &surface;
    info.surfaces_count = 1;
    remixapi_MeshHandle result;
    r.createMesh(info, result);
    return result;
  }
  inline remixapi_InstanceInfo instance(remixapi_MeshHandle mesh, float z = 0) {
    remixapi_InstanceInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    info.mesh = mesh;
    info.transform.matrix[0][0] = info.transform.matrix[1][1] = info.transform.matrix[2][2] = 1;
    info.transform.matrix[2][3] = z;
    return info;
  }
  inline void camera(remix::NativeResources &r) {
    remixapi_CameraInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    for (int i = 0; i < 4; ++i) {
      info.view[i][i] = info.projection[i][i] = 1;
    }
    r.setupCamera(info);
  }
  inline void ui(remix::NativeResources &r, uint64_t texture, uint32_t color, bool depth = false, float z = 0) {
    const remixapi_UIVertex vertices[] = {
        {16, 16, z, 0, 0, color}, {48, 16, z, 1, 0, color}, {48, 48, z, 1, 1, color}, {16, 48, z, 0, 1, color}};
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    const remixapi_UIDrawCommand command = {texture, 6, 0, 0, depth ? REMIXAPI_UI_DRAW_FLAG_DEPTH_TEST : 0u};
    remixapi_UIDrawList list = {};
    list.sType = REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST;
    list.displayWidth = list.displayHeight = 64;
    list.pVertices = vertices;
    list.vertexCount = 4;
    list.pIndices = indices;
    list.indexCount = 6;
    list.pCommands = &command;
    list.commandCount = 1;
    r.submitUi(&list);
  }

} // namespace remix::test
