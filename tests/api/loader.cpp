#include <remix/remix_c.h>

#include <cstring>

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    return 1;
  }

  remixapi_Interface remix = {};
  remixapi_HMODULE library = nullptr;
  if (remixapi_lib_loadRemixDllAndInitialize(argv[1], &remix, &library) != REMIXAPI_ERROR_CODE_SUCCESS) {
    return 2;
  }
  if (library == nullptr || remix.Shutdown == nullptr) {
    return 3;
  }
  if (remix.GetFeatureAvailability != nullptr) {
    remixapi_FeatureAvailability features = {};
    if (remix.GetFeatureAvailability(&features) != REMIXAPI_ERROR_CODE_SUCCESS) {
      return 4;
    }
  }
  if (remix.GetWindowState != nullptr) {
    remixapi_WindowState state = {};
    if (remix.GetWindowState(&state) != REMIXAPI_ERROR_CODE_NOT_INITIALIZED) {
      return 5;
    }
  }
  if (argc == 3) {
    if (remix.PumpEvents == nullptr || remix.GetWindowState == nullptr || remix.PollMouseState == nullptr ||
        remix.IsKeyDown == nullptr || remix.SetMouseGrabbed == nullptr || remix.SetCursorPosition == nullptr ||
        remix.SetFullscreen == nullptr || remix.GetFeatureAvailability == nullptr || remix.SetGameValue == nullptr ||
        remix.GetGameValue == nullptr || remix.CreateMaterial == nullptr) {
      return 8;
    }
    if (remix.PumpEvents() != REMIXAPI_ERROR_CODE_NOT_INITIALIZED) {
      return 9;
    }
    if (remix.SetGameValue("test.value", "native") != REMIXAPI_ERROR_CODE_SUCCESS) {
      return 10;
    }
    char value[16] = {};
    uint32_t valueSize = 0;
    if (remix.GetGameValue("test.value", value, sizeof(value), &valueSize) != REMIXAPI_ERROR_CODE_SUCCESS ||
        valueSize != 7 || std::strcmp(value, "native") != 0) {
      return 11;
    }
    remixapi_MaterialInfo material = {};
    material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    remixapi_MaterialHandle materialHandle = nullptr;
    if (remix.CreateMaterial(&material, &materialHandle) != REMIXAPI_ERROR_CODE_NOT_INITIALIZED ||
        materialHandle != nullptr) {
      return 12;
    }
  }
  if (remixapi_lib_shutdownAndUnloadRemixDll(&remix, library) != REMIXAPI_ERROR_CODE_SUCCESS) {
    return 6;
  }
  return remix.Shutdown == nullptr ? 0 : 7;
}
