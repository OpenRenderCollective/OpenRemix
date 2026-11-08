#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <remix/remix_usd_ext.h>
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
int main(int argc, char **argv) {
  require(argc == 2, "fixture path");
  remixapi_USDLoadOptionsEXT options{sizeof(options), 1, argv[1], 0, 0, 0};
  remixapi_USDSceneHandleEXT scene = 0;
  // Loading and diagnostics must work without Startup or a Vulkan device.
  require(remixapi_LoadUSDSceneEXT(&options, &scene) == REMIXAPI_ERROR_CODE_SUCCESS && scene,
          "CPU-only extension load");
  remixapi_USDDiagnosticsEXT diagnostics{};
  diagnostics.structSize = sizeof(diagnostics);
  require(!remixapi_GetUSDDiagnosticsEXT(scene, &diagnostics) && diagnostics.version == 1 && diagnostics.meshes == 2 &&
              diagnostics.generation == 1,
          "versioned scene diagnostics");
  require(!remixapi_ReloadUSDSceneEXT(scene), "schedule asynchronous reload");
  require(!remixapi_DestroyUSDSceneEXT(scene), "destroy joins parser without a renderer");
  require(remixapi_GetUSDDiagnosticsEXT(scene, &diagnostics) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "destroyed handle rejected");
  options.version = 2;
  require(remixapi_LoadUSDSceneEXT(&options, &scene) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS && scene == 0,
          "unknown version rejected");
  options.version = 1;
  options.path = "missing-openremix-api-test.usda";
  require(remixapi_LoadUSDSceneEXT(&options, &scene) == REMIXAPI_ERROR_CODE_GENERAL_FAILURE && scene == 0,
          "failed load returns no handle");
  require(!remixapi_GetUSDDiagnosticsEXT(0, &diagnostics) && std::strlen(diagnostics.message) > 0,
          "failed load diagnostic can be retrieved");
  std::puts("CPU-only versioned USD API passed");
}
