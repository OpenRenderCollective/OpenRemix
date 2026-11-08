#include "remix-native/scene.h"
#include "runtime/prepared.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void ok(remixapi_ErrorCode result) { require(!result, "Process-exit fixture API failure"); }
} // namespace

int main(int argc, char **argv) {
  require(argc == 2, "Provide reconstruction mode");
  const bool sr = std::strcmp(argv[1], "sr") == 0;
  const bool rr = std::strcmp(argv[1], "rr") == 0;
  const bool fsr = std::strcmp(argv[1], "fsr31") == 0;
  remixapi_InitializeLibraryInfo library{};
  library.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  library.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  remixapi_Interface api{};
  ok(remixapi_InitializeLibrary(&library, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
  startup.windowWidth = 640;
  startup.windowHeight = 480;
  ok(api.Startup(&startup));
  SampleScene scene;
  ok(scene.create(api));
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(scene.draw(api, 0, 640, 480));
    ok(api.Present(nullptr));
  });
  // First initialize the raster/validation exit handler, then load the tracing
  // and reconstruction SDK resources asynchronously after it was registered.
  frame();
  ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", sr || rr ? "dlss" : argv[1]));
  ok(api.SetConfigVariable("rtx.native.pathtrace.dlssRayReconstruction", rr ? "1" : "0"));
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  for (int i = 0; i < 8; ++i)
    frame();
  const auto reconstruction = remix::api::g_runtime.reconstructionStatistics();
  if ((sr && !reconstruction.srSupported) || (rr && !reconstruction.rrSupported)) {
    ok(api.Shutdown());
    return 77;
  }
  require(remix::api::g_runtime.backendCapabilities().activeBackend == 1, "Tracing must be active at exit");
  require(uint32_t(reconstruction.backend) == (rr    ? 3u
                                               : sr  ? 2u
                                               : fsr ? 1u
                                                     : 0u),
          "Requested reconstruction must be active at exit");
  std::fprintf(stderr, "Exit without explicit Shutdown after active %s frames\n", argv[1]);
  // Deliberately leave live device/SDK resources for process-exit cleanup.
  return 0;
}
