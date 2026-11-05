// Host passes (remixapi_HostPassEXT) on a real device: shaders registered,
// a draw into a host target, a copy and a draw on the frame sampling the
// target, with push constants and a uniform buffer; the statistics say every
// pass ran once its pipelines compiled, and a screenshot shows the result.
#include <remix/remix_c.h>
#include <remix/remix_native_ext.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
remixapi_Interface api = {};
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
void checked(remixapi_ErrorCode code, int line) {
  if (code != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::fprintf(stderr, "FAIL: status %d at line %d\n", int(code), line);
    std::exit(1);
  }
}
#define ok(code) checked((code), __LINE__)
void *extension(remixapi_HMODULE library, const char *name) {
#if defined(_WIN32)
  return reinterpret_cast<void *>(GetProcAddress(library, name));
#else
  return dlsym(library, name);
#endif
}

// glslangValidator -V --target-env vulkan1.0 of:
//   layout(location = 0) in vec2 position; layout(location = 0) out vec2 uv;
//   gl_Position = vec4(position, 0, 1); uv = position * 0.5 + 0.5;
const uint32_t kVertex[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000022, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000012, 0x0000001c,
    0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005,
    0x0000000b, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000000b, 0x00000000,
    0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000000b, 0x00000001, 0x505f6c67, 0x746e696f,
    0x657a6953, 0x00000000, 0x00070006, 0x0000000b, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369,
    0x0065636e, 0x00070006, 0x0000000b, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e,
    0x00030005, 0x0000000d, 0x00000000, 0x00050005, 0x00000012, 0x69736f70, 0x6e6f6974, 0x00000000,
    0x00030005, 0x0000001c, 0x00007675, 0x00030047, 0x0000000b, 0x00000002, 0x00050048, 0x0000000b,
    0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000000b, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x0000000b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000000b, 0x00000003,
    0x0000000b, 0x00000004, 0x00040047, 0x00000012, 0x0000001e, 0x00000000, 0x00040047, 0x0000001c,
    0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040015, 0x00000008,
    0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c, 0x0000000a,
    0x00000006, 0x00000009, 0x0006001e, 0x0000000b, 0x00000007, 0x00000006, 0x0000000a, 0x0000000a,
    0x00040020, 0x0000000c, 0x00000003, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000003,
    0x00040015, 0x0000000e, 0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000,
    0x00040017, 0x00000010, 0x00000006, 0x00000002, 0x00040020, 0x00000011, 0x00000001, 0x00000010,
    0x0004003b, 0x00000011, 0x00000012, 0x00000001, 0x0004002b, 0x00000006, 0x00000014, 0x00000000,
    0x0004002b, 0x00000006, 0x00000015, 0x3f800000, 0x00040020, 0x00000019, 0x00000003, 0x00000007,
    0x00040020, 0x0000001b, 0x00000003, 0x00000010, 0x0004003b, 0x0000001b, 0x0000001c, 0x00000003,
    0x0004002b, 0x00000006, 0x0000001e, 0x3f000000, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000010, 0x00000013, 0x00000012, 0x00050051,
    0x00000006, 0x00000016, 0x00000013, 0x00000000, 0x00050051, 0x00000006, 0x00000017, 0x00000013,
    0x00000001, 0x00070050, 0x00000007, 0x00000018, 0x00000016, 0x00000017, 0x00000014, 0x00000015,
    0x00050041, 0x00000019, 0x0000001a, 0x0000000d, 0x0000000f, 0x0003003e, 0x0000001a, 0x00000018,
    0x0004003d, 0x00000010, 0x0000001d, 0x00000012, 0x0005008e, 0x00000010, 0x0000001f, 0x0000001d,
    0x0000001e, 0x00050050, 0x00000010, 0x00000020, 0x0000001e, 0x0000001e, 0x00050081, 0x00000010,
    0x00000021, 0x0000001f, 0x00000020, 0x0003003e, 0x0000001c, 0x00000021, 0x000100fd, 0x00010038,
};
//   layout(set = 0, binding = 10) uniform Constants { vec4 color; vec4 weights; } c;
//   layout(set = 0, binding = 13) uniform sampler2D source;
//   layout(push_constant) uniform Push { vec4 add; } p;
//   result = c.color * c.weights.x + texture(source, uv) * c.weights.y + p.add;
const uint32_t kFragment[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000002f, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00000020, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00040005, 0x00000009, 0x75736572, 0x0000746c, 0x00050005, 0x0000000a, 0x736e6f43,
    0x746e6174, 0x00000073, 0x00050006, 0x0000000a, 0x00000000, 0x6f6c6f63, 0x00000072, 0x00050006,
    0x0000000a, 0x00000001, 0x67696577, 0x00737468, 0x00030005, 0x0000000c, 0x00000063, 0x00040005,
    0x0000001c, 0x72756f73, 0x00006563, 0x00030005, 0x00000020, 0x00007675, 0x00040005, 0x00000028,
    0x68737550, 0x00000000, 0x00040006, 0x00000028, 0x00000000, 0x00646461, 0x00030005, 0x0000002a,
    0x00000070, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00030047, 0x0000000a, 0x00000002,
    0x00050048, 0x0000000a, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x0000000a, 0x00000001,
    0x00000023, 0x00000010, 0x00040047, 0x0000000c, 0x00000021, 0x0000000a, 0x00040047, 0x0000000c,
    0x00000022, 0x00000000, 0x00040047, 0x0000001c, 0x00000021, 0x0000000d, 0x00040047, 0x0000001c,
    0x00000022, 0x00000000, 0x00040047, 0x00000020, 0x0000001e, 0x00000000, 0x00030047, 0x00000028,
    0x00000002, 0x00050048, 0x00000028, 0x00000000, 0x00000023, 0x00000000, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000003, 0x0004001e, 0x0000000a, 0x00000007, 0x00000007, 0x00040020, 0x0000000b,
    0x00000002, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000002, 0x00040015, 0x0000000d,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000d, 0x0000000e, 0x00000000, 0x00040020, 0x0000000f,
    0x00000002, 0x00000007, 0x0004002b, 0x0000000d, 0x00000012, 0x00000001, 0x00040015, 0x00000013,
    0x00000020, 0x00000000, 0x0004002b, 0x00000013, 0x00000014, 0x00000000, 0x00040020, 0x00000015,
    0x00000002, 0x00000006, 0x00090019, 0x00000019, 0x00000006, 0x00000001, 0x00000000, 0x00000000,
    0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x0000001a, 0x00000019, 0x00040020, 0x0000001b,
    0x00000000, 0x0000001a, 0x0004003b, 0x0000001b, 0x0000001c, 0x00000000, 0x00040017, 0x0000001e,
    0x00000006, 0x00000002, 0x00040020, 0x0000001f, 0x00000001, 0x0000001e, 0x0004003b, 0x0000001f,
    0x00000020, 0x00000001, 0x0004002b, 0x00000013, 0x00000023, 0x00000001, 0x0003001e, 0x00000028,
    0x00000007, 0x00040020, 0x00000029, 0x00000009, 0x00000028, 0x0004003b, 0x00000029, 0x0000002a,
    0x00000009, 0x00040020, 0x0000002b, 0x00000009, 0x00000007, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x00050041, 0x0000000f, 0x00000010, 0x0000000c,
    0x0000000e, 0x0004003d, 0x00000007, 0x00000011, 0x00000010, 0x00060041, 0x00000015, 0x00000016,
    0x0000000c, 0x00000012, 0x00000014, 0x0004003d, 0x00000006, 0x00000017, 0x00000016, 0x0005008e,
    0x00000007, 0x00000018, 0x00000011, 0x00000017, 0x0004003d, 0x0000001a, 0x0000001d, 0x0000001c,
    0x0004003d, 0x0000001e, 0x00000021, 0x00000020, 0x00050057, 0x00000007, 0x00000022, 0x0000001d,
    0x00000021, 0x00060041, 0x00000015, 0x00000024, 0x0000000c, 0x00000012, 0x00000023, 0x0004003d,
    0x00000006, 0x00000025, 0x00000024, 0x0005008e, 0x00000007, 0x00000026, 0x00000022, 0x00000025,
    0x00050081, 0x00000007, 0x00000027, 0x00000018, 0x00000026, 0x00050041, 0x0000002b, 0x0000002c,
    0x0000002a, 0x0000000e, 0x0004003d, 0x00000007, 0x0000002d, 0x0000002c, 0x00050081, 0x00000007,
    0x0000002e, 0x00000027, 0x0000002d, 0x0003003e, 0x00000009, 0x0000002e, 0x000100fd, 0x00010038,
};
} // namespace

int main(int argc, char **argv) {
  require(argc >= 2, "Provide runtime library");
  remixapi_HMODULE library = nullptr;
#if defined(_WIN32)
  std::wstring path(argv[1], argv[1] + std::strlen(argv[1]));
  auto libraryPath = path.c_str();
#else
  auto libraryPath = argv[1];
#endif
  ok(remixapi_lib_loadRemixDllAndInitialize(libraryPath, &api, &library));
  auto registerShader = reinterpret_cast<PFN_remixapi_RegisterHostShaderEXT>(
      extension(library, "remixapi_RegisterHostShaderEXT"));
  auto destroyShader = reinterpret_cast<PFN_remixapi_DestroyHostShaderEXT>(
      extension(library, "remixapi_DestroyHostShaderEXT"));
  auto submit = reinterpret_cast<PFN_remixapi_SubmitHostPassesEXT>(
      extension(library, "remixapi_SubmitHostPassesEXT"));
  auto statistics = reinterpret_cast<PFN_remixapi_GetHostPassStatisticsEXT>(
      extension(library, "remixapi_GetHostPassStatisticsEXT"));
  auto updateTarget = reinterpret_cast<PFN_remixapi_UpdateHostTargetEXT>(
      extension(library, "remixapi_UpdateHostTargetEXT"));
  auto updateVolume = reinterpret_cast<PFN_remixapi_UpdateHostVolumeEXT>(
      extension(library, "remixapi_UpdateHostVolumeEXT"));
  auto passesEnabled = reinterpret_cast<PFN_remixapi_GetHostPassesEnabledEXT>(
      extension(library, "remixapi_GetHostPassesEnabledEXT"));
  require(registerShader && destroyShader && submit && statistics && updateTarget && updateVolume && passesEnabled,
          "Host pass exports");
  remixapi_HostShaderInfoEXT info = {};
  info.sType = REMIXAPI_STRUCT_TYPE_HOST_SHADER_INFO_EXT;
  remixapi_HostShaderHandleEXT vertex = 0, fragment = 0;
  require(registerShader(&info, &vertex) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED, "Register before startup");
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = 640;
  startup.windowHeight = 480;
#endif
  ok(api.Startup(&startup));
  // The capture shows the passes' colours in SDR (an HDR display would turn on HDR10 output).
  const auto setDefault = reinterpret_cast<remixapi_ErrorCode(REMIXAPI_CALL *)(const char *, const char *)>(
      extension(library, "remixapi_SetConfigDefaultEXT"));
  require(setDefault, "SetConfigDefaultEXT is not exported");
  ok(setDefault("rtx.native.hdr", "0"));
  if (const auto status = api.SetConfigVariable("rtx.native.backend", "raster");
      status != REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE)
    ok(status);
  ok(api.Present(nullptr));
  // Shaders: the vertex one has an attribute at location 0; the fragment one
  // a uniform buffer at 10, a sampler at 13 and 16 bytes of push constants.
  info.stage = REMIXAPI_HOST_SHADER_VERTEX_EXT;
  info.code = kVertex;
  info.codeBytes = sizeof(kVertex);
  ok(registerShader(&info, &vertex));
  const remixapi_HostShaderBindingEXT bindings[] = {{10, REMIXAPI_HOST_BINDING_UNIFORM_BUFFER_EXT},
                                                    {13, REMIXAPI_HOST_BINDING_SAMPLER_EXT}};
  info.stage = REMIXAPI_HOST_SHADER_FRAGMENT_EXT;
  info.code = kFragment;
  info.codeBytes = sizeof(kFragment);
  info.bindings = bindings;
  info.bindingCount = 2;
  info.pushConstantBytes = 16;
  ok(registerShader(&info, &fragment));
  require(vertex && fragment && vertex != fragment, "Shader handles");
  info.codeBytes = 7;
  remixapi_HostShaderHandleEXT bad = 1;
  require(registerShader(&info, &bad) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS && !bad, "Odd code size rejected");

  // A full-screen quad (two triangles) by indices.
  const float quad[] = {-1, -1, 1, -1, 1, 1, -1, 1};
  const uint32_t indices[] = {0, 1, 2, 0, 2, 3};
  const remixapi_HostPassVertexBindingEXT vertexBinding = {quad, sizeof(quad), 8};
  const remixapi_HostPassAttributeEXT attribute = {0, 0, 103 /* VK_FORMAT_R32G32_SFLOAT */, 0};
  auto draw = [&](remixapi_HostPassEXT &pass, remixapi_HostTargetHandleEXT target, uint32_t width, uint32_t height,
                  const float *constants, const remixapi_HostPassBufferEXT *buffer, const float *push,
                  const remixapi_HostPassSamplerEXT *sampler) {
    pass = {};
    pass.kind = REMIXAPI_HOST_PASS_DRAW_EXT;
    pass.target = target;
    pass.targetWidth = width;
    pass.targetHeight = height;
    pass.vertexShader = vertex;
    pass.fragmentShader = fragment;
    pass.buffers = buffer;
    pass.bufferCount = 1;
    pass.samplers = sampler;
    pass.samplerCount = 1;
    pass.pushConstants = push;
    pass.pushConstantBytes = 16;
    pass.vertexBindings = &vertexBinding;
    pass.vertexBindingCount = 1;
    pass.attributes = &attribute;
    pass.attributeCount = 1;
    pass.indices = indices;
    pass.indexCount = 6;
    pass.srcColorBlend = pass.srcAlphaBlend = 1; // ONE
    pass.colorWriteMask = 0xf;
    (void)constants;
  };
  // Pass 1: the target (320x240, the frame's format) cleared to red.
  // Pass 2: a draw into it: green (color * 1 + source * 0 + 0), over the left
  //         half only (viewport), so the right half stays red.
  // Pass 3: the frame copied into another target (no use but the copy path).
  // Pass 4: a draw on the frame: the target sampled (color * 0 + source * 1)
  //         plus a blue push constant: the left half (0,1,1), the right (1,0,1).
  const float green[8] = {0, 1, 0, 1, 1, 0, 0, 0}, sample[8] = {0, 0, 0, 0, 0, 1, 0, 0};
  const float none[4] = {0, 0, 0, 0}, blue[4] = {0, 0, 1, 0};
  const remixapi_HostPassBufferEXT greenBuffer = {10, green, sizeof(green)}, sampleBuffer = {10, sample, sizeof(sample)};
  const remixapi_HostPassSamplerEXT frameSampler = {13, 0, 1, 1, 1}, targetSampler = {13, 7, 1, 1, 1};
  remixapi_HostPassEXT passes[4] = {};
  passes[0].kind = REMIXAPI_HOST_PASS_CLEAR_EXT;
  passes[0].target = 7;
  passes[0].targetWidth = 320;
  passes[0].targetHeight = 240;
  passes[0].clearColor[0] = passes[0].clearColor[3] = 1;
  draw(passes[1], 7, 320, 240, green, &greenBuffer, none, &frameSampler);
  passes[1].viewport[2] = 160;
  passes[1].viewport[3] = 240;
  passes[2].kind = REMIXAPI_HOST_PASS_COPY_EXT;
  passes[2].target = 8;
  passes[2].targetWidth = 64;
  passes[2].targetHeight = 64;
  passes[2].linear = 1;
  draw(passes[3], 0, 0, 0, sample, &sampleBuffer, blue, &targetSampler);
  // A texture from memory (2x2 white, RGBA) as target 9: uploaded before the
  // passes, a fourth live target.
  const uint8_t white[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
  require(updateTarget(9, 2, 2, 37 /* VK_FORMAT_R8G8B8A8_UNORM */, white, 15) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Short upload rejected");
  ok(updateTarget(9, 2, 2, 37, white, sizeof(white)));
  remixapi_HostPassListEXT list = {};
  list.sType = REMIXAPI_STRUCT_TYPE_HOST_PASS_LIST_EXT;
  list.passes = passes;
  list.passCount = 4;
  remixapi_HostPassStatisticsEXT stats = {};
  stats.structSize = sizeof(stats);
  // The setting (rtx.native.hostPasses) is off by default: submissions are
  // accepted and dropped, nothing runs. On, they run from the next submission.
  remixapi_Bool hostPassesOn = 1;
  ok(passesEnabled(&hostPassesOn));
  require(!hostPassesOn, "Host passes off by default");
  ok(submit(&list));
  ok(api.Present(nullptr));
  ok(statistics(&stats));
  require(stats.passes == 0, "Host passes off: submission dropped");
  // Passes flagged REMIXAPI_HOST_PASS_ALWAYS_EXT run regardless (the clear
  // here); the rest stay dropped.
  const uint32_t clearAlways[1] = {REMIXAPI_HOST_PASS_ALWAYS_EXT};
  remixapi_HostPassListFlagsEXT listFlags = {};
  listFlags.sType = REMIXAPI_STRUCT_TYPE_HOST_PASS_LIST_FLAGS_EXT;
  listFlags.flags = clearAlways;
  listFlags.flagCount = 1;
  list.pNext = &listFlags;
  ok(submit(&list));
  ok(api.Present(nullptr));
  ok(statistics(&stats));
  require(stats.passes == 1 && stats.clears == 1 && stats.always == 1 && stats.dropped == 3,
          "Host passes off: the always pass ran");
  list.pNext = nullptr;
  ok(api.SetConfigVariable("rtx.native.hostPasses", "1"));
  ok(passesEnabled(&hostPassesOn));
  require(hostPassesOn, "Host passes on");
  ok(updateTarget(9, 2, 2, 37, white, sizeof(white))); // again (the earlier one went up with the always pass)
  // Pipelines compile in the background: the draws are skipped until ready.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  bool complete = false;
  for (int frame = 0; frame < 2000 && !complete; ++frame) {
    ok(submit(&list));
    ok(api.Present(nullptr));
    ok(statistics(&stats));
    require(stats.passes == 4 && stats.clears == 1 && stats.copies == 1, "Clear and copy ran");
    require(stats.draws + stats.skipped == 2, "Draw accounting");
    complete = stats.draws == 2;
    require(std::chrono::steady_clock::now() < deadline, "Host pass pipelines never compiled");
    if (!complete)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Both draws share the shaders, state and target format: one pipeline.
  std::printf("statistics: %u pipelines, %u shaders, %u targets\n", stats.pipelines, stats.shaders, stats.targets);
  require(stats.pipelines == 1 && stats.shaders == 2 && stats.targets == 4, "Statistics");
  std::printf("host passes ran: %u draws, %u pipelines, %u targets\n", stats.draws, stats.pipelines, stats.targets);
  if (argc > 2) {
    ok(api.RequestPresentedScreenshot(argv[2]));
    for (int frame = 0; frame < 5; ++frame) {
      ok(submit(&list));
      ok(api.Present(nullptr));
    }
  }
  // The runtime's view depth (REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT): sampled
  // only, and absent on the raster backend, which skips the draw.
  const remixapi_HostPassSamplerEXT depthSampler = {13, REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT, 0, 1, 1};
  remixapi_HostPassEXT depthPass = {};
  draw(depthPass, 0, 0, 0, sample, &sampleBuffer, blue, &depthSampler);
  list.passes = &depthPass;
  list.passCount = 1;
  ok(submit(&list));
  ok(api.Present(nullptr));
  ok(statistics(&stats));
  require(stats.passes == 1 && stats.draws == 0 && stats.skipped == 1, "View depth absent: draw skipped");
  depthPass.target = REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT;
  depthPass.targetWidth = depthPass.targetHeight = 64;
  require(submit(&list) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "Reserved target refused");
  require(updateTarget(REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT, 2, 2, 37, white, sizeof(white)) ==
              REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Reserved target not updatable");
  // A volume (2x2x2 white) as target 11: sampled only; a draw into it is skipped.
  uint8_t volume[32];
  std::memset(volume, 255, sizeof(volume));
  require(updateVolume(11, 2, 2, 2, 37, volume, 16) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "Short volume rejected");
  ok(updateVolume(11, 2, 2, 2, 37, volume, sizeof(volume)));
  const remixapi_HostPassSamplerEXT volumeSampler = {13, 11, 1, 1, 1};
  remixapi_HostPassEXT volumePass = {};
  draw(volumePass, 11, 2, 2, sample, &sampleBuffer, blue, &volumeSampler);
  list.passes = &volumePass;
  list.passCount = 1;
  ok(submit(&list));
  ok(api.Present(nullptr));
  ok(statistics(&stats));
  require(stats.passes == 1 && stats.draws == 0 && stats.skippedTarget == 1, "Draw into a volume skipped");
  // The frame's depth (REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT): a depth-only
  // draw fills it, then a draw on the frame tests against it.
  remixapi_HostPassEXT depthPasses[2] = {};
  draw(depthPasses[0], REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT, 0, 0, green, &greenBuffer, none, &frameSampler);
  depthPasses[0].depth = 8 /* VK_COMPARE_OP_ALWAYS + 1 */ | REMIXAPI_HOST_DEPTH_WRITE_EXT;
  draw(depthPasses[1], 0, 0, 0, sample, &sampleBuffer, blue, &targetSampler);
  depthPasses[1].depth = 4 /* VK_COMPARE_OP_LESS_OR_EQUAL + 1 */;
  list.passes = depthPasses;
  list.passCount = 2;
  for (int frame = 0; frame < 2000; ++frame) {
    ok(submit(&list));
    ok(api.Present(nullptr));
    ok(statistics(&stats));
    require(stats.passes == 2 && stats.draws + stats.skipped == 2 && stats.skippedTarget == 0, "Depth draws accounted");
    if (stats.draws == 2)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  require(stats.draws == 2, "Depth-only draw and depth-tested draw ran");
  depthPasses[1].target = 7;
  depthPasses[1].targetWidth = 320;
  depthPasses[1].targetHeight = 240;
  require(submit(&list) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "Depth test on a host target refused");
  list.passes = passes;
  // Nothing submitted: nothing runs. A destroyed shader's draws are refused.
  list.passCount = 0;
  ok(submit(&list));
  ok(api.Present(nullptr));
  ok(statistics(&stats));
  require(stats.passes == 0 && stats.draws == 0, "Empty submission");
  ok(destroyShader(fragment));
  require(destroyShader(fragment) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "Double destroy");
  list.passCount = 4;
  require(submit(&list) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "Destroyed shader refused");
  ok(api.Present(nullptr));
  ok(statistics(&stats));
  require(stats.passes == 0 && stats.shaders == 1, "Refused submission ran nothing");
  ok(remixapi_lib_shutdownAndUnloadRemixDll(&api, library));
  std::printf("host pass test passed\n");
  return 0;
}
