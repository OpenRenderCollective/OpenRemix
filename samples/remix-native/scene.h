#pragma once
#include <array>
#include <cmath>
#include <remix/remix_native_ext.h>

// A scene built entirely through the public SDK, also used by lifecycle tests.
struct SampleScene {
  remixapi_LightHandle daylight = nullptr;
  remixapi_MeshHandle mesh = nullptr;
  remixapi_MaterialHandle material = nullptr;
  remixapi_TextureHandle texture = nullptr;
  remixapi_ErrorCode create(remixapi_Interface &api) {
    remixapi_LightInfoDistantEXT distant = {};
    distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
    distant.direction = {-.4f, -.8f, -.3f};
    remixapi_LightInfo light = {};
    light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
    light.pNext = &distant;
    light.hash = 0x104;
    light.radiance = {3.14159265f, 3.14159265f, 3.14159265f};
    auto lightResult = api.CreateLight(&light, &daylight);
    if (lightResult)
      return lightResult;
    std::array<uint32_t, 64> pixels;
    for (unsigned y = 0; y < 8; ++y) {
      for (unsigned x = 0; x < 8; ++x) {
        pixels[y * 8 + x] = ((x / 2 + y / 2) % 2) ? 0xffe5b33a : 0xfff4f0db;
      }
    }
    remixapi_TextureInfo tex = {};
    tex.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    tex.hash = 0x100;
    tex.width = tex.height = 8;
    tex.depth = tex.mipLevels = 1;
    tex.format = REMIXAPI_FORMAT_R8G8B8A8_SRGB;
    tex.data = pixels.data();
    tex.dataSize = sizeof(pixels);
    auto result = api.CreateTexture(&tex, &texture);
    if (result) {
      return result;
    }
    result = api.RegisterUITexture(1, 8, 8, tex.format, pixels.data(), sizeof(pixels));
    if (result) {
      return result;
    }
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo mat = {};
    mat.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    mat.pNext = &opaque;
    mat.hash = 0x101;
#if defined(_WIN32)
    mat.albedoTexture = L"0x100";
#else
    mat.albedoTexture = "0x100";
#endif
    mat.wrapModeU = mat.wrapModeV = 1;
    result = api.CreateMaterial(&mat, &material);
    if (result) {
      return result;
    }
    constexpr float corners[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                     {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    constexpr unsigned faces[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 4, 7, 3},
                                      {1, 2, 6, 5}, {3, 7, 6, 2}, {0, 1, 5, 4}};
    constexpr uint32_t colors[] = {0xffffffff, 0xff70a0ff, 0xffff9070, 0xffa0ff90, 0xffffffff, 0xff90a0b0};
    std::array<remixapi_HardcodedVertex, 24> vertices = {};
    std::array<uint32_t, 36> indices;
    for (unsigned f = 0; f < 6; ++f) {
      for (unsigned v = 0; v < 4; ++v) {
        for (unsigned c = 0; c < 3; ++c) {
          vertices[f * 4 + v].position[c] = corners[faces[f][v]][c];
        }
        vertices[f * 4 + v].texcoord[0] = v >= 2 ? 1.0f : 0.0f;
        vertices[f * 4 + v].texcoord[1] = v == 1 || v == 2 ? 1.0f : 0.0f;
        vertices[f * 4 + v].color = colors[f];
      }
      constexpr unsigned triangle[] = {0, 1, 2, 0, 2, 3};
      for (unsigned i = 0; i < 6; ++i) {
        indices[f * 6 + i] = f * 4 + triangle[i];
      }
    }
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.vertices_values = vertices.data();
    surface.vertices_count = vertices.size();
    surface.indices_values = indices.data();
    surface.indices_count = indices.size();
    surface.material = material;
    remixapi_MeshInfo info = {};
    info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    info.hash = 0x102;
    info.surfaces_count = 1;
    info.surfaces_values = &surface;
    return api.CreateMeshBatched(&info, &mesh);
  }
  remixapi_ErrorCode draw(remixapi_Interface &api, float seconds, uint32_t width, uint32_t height) {
    auto lightResult = api.DrawLightInstance(daylight);
    if (lightResult)
      return lightResult;
    remixapi_CameraInfoParameterizedEXT params = {};
    params.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
    params.forward = {0, 0, 1};
    params.up = {0, 1, 0};
    params.right = {1, 0, 0};
    params.aspect = height ? float(width) / height : 1;
    params.fovYInDegrees = 60;
    params.nearPlane = .05f;
    params.farPlane = 100;
    remixapi_CameraInfo camera = {};
    camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
    camera.pNext = &params;
    auto result = api.SetupCamera(&camera);
    if (result) {
      return result;
    }
    remixapi_InstanceInfo instance = {};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.mesh = mesh;
    instance.transform.matrix[0][0] = std::cos(seconds);
    instance.transform.matrix[0][2] = std::sin(seconds);
    instance.transform.matrix[1][1] = 1;
    instance.transform.matrix[2][0] = -std::sin(seconds);
    instance.transform.matrix[2][2] = std::cos(seconds);
    instance.transform.matrix[2][3] = 6;
    result = api.DrawInstance(&instance);
    if (result) {
      return result;
    }
    instance.transform.matrix[0][3] = 2.5f;
    instance.transform.matrix[2][3] = 9;
    result = api.DrawInstance(&instance);
    if (result) {
      return result;
    }
    // Colored HUD bar and textured icon, independent of the debug panel.
    const remixapi_UIVertex vertices[] = {{20, 400, 0, 0, 0, 0xd9302010},  {620, 400, 0, 1, 0, 0xd9302010},
                                          {620, 460, 0, 1, 1, 0xd9302010}, {20, 460, 0, 0, 1, 0xd9302010},
                                          {32, 410, 0, 0, 0, 0xffffffff},  {72, 410, 0, 1, 0, 0xffffffff},
                                          {72, 450, 0, 1, 1, 0xffffffff},  {32, 450, 0, 0, 1, 0xffffffff}};
    const uint32_t indices[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    const remixapi_UIDrawCommand commands[] = {{0, 6, 0, 0, 0}, {1, 6, 6, 0, 0}};
    remixapi_UIDrawList ui = {};
    ui.sType = REMIXAPI_STRUCT_TYPE_UI_DRAW_LIST;
    ui.displayWidth = 640;
    ui.displayHeight = 480;
    ui.pVertices = vertices;
    ui.vertexCount = 8;
    ui.pIndices = indices;
    ui.indexCount = 12;
    ui.pCommands = commands;
    ui.commandCount = 2;
    return api.SubmitUIDrawList(&ui);
  }
};
