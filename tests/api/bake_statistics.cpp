#include <remix/remix_bake_ext.h>
#include <remix/remix_native_ext.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void ok(remixapi_ErrorCode code) {
    require(code == REMIXAPI_ERROR_CODE_SUCCESS, "Bake statistics API failed");
  }
}

int main() {
  static_assert(sizeof(remixapi_BackendCapabilitiesEXT) == 272);
  remixapi_PreparationStatisticsEXT preparation{};
  require(remixapi_GetPreparationStatisticsEXT(nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS &&
              remixapi_GetPreparationStatisticsEXT(&preparation) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Preparation statistics validate pointer and exact independent layout");
  preparation.structSize = sizeof(preparation);
  require(remixapi_GetPreparationStatisticsEXT(&preparation) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED,
          "Preparation statistics require startup");
  static_assert(offsetof(remixapi_BakeStatisticsEXT, reflectionRequested) == 256);
  remixapi_BakeStatisticsEXT statistics{};
  require(remixapi_GetBakeStatisticsEXT(nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Null statistics rejected");
  require(remixapi_GetBakeStatisticsEXT(&statistics) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid statistics size rejected");
  statistics.structSize = sizeof(statistics);
  require(remixapi_GetBakeStatisticsEXT(&statistics) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED,
          "Statistics require startup");

  remixapi_InitializeLibraryInfo initialize{};
  initialize.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  initialize.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  remixapi_Interface api{};
  ok(remixapi_InitializeLibrary(&initialize, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = 320;
  startup.windowHeight = 240;
#endif
  ok(api.Startup(&startup));
  ok(remixapi_GetPreparationStatisticsEXT(&preparation));
  require(preparation.structSize == sizeof(preparation) && preparation.pending <= 1 &&
              preparation.loading <= 1 && preparation.failed <= 1 &&
              preparation.pipelinesCompleted <= preparation.pipelinesRequested &&
              preparation.failureReason[255] == 0,
          "Preparation diagnostics return bounded flags, counters and terminated failure text");

  statistics.structSize = 256;
  statistics.reflectionRequested = statistics.reflectionPending =
      statistics.reflectionActive = statistics.reflectionFailed = 0xa5a5a5a5;
  ok(remixapi_GetBakeStatisticsEXT(&statistics));
  require(statistics.reflectionRequested == 0xa5a5a5a5 && statistics.reflectionPending == 0xa5a5a5a5 &&
              statistics.reflectionActive == 0xa5a5a5a5 && statistics.reflectionFailed == 0xa5a5a5a5,
          "Legacy statistics callers retain their allocation boundary");
  statistics = {};
  statistics.structSize = sizeof(statistics);
  ok(remixapi_GetBakeStatisticsEXT(&statistics));
  require(statistics.structSize == sizeof(statistics) && !statistics.reflectionRequested &&
              !statistics.reflectionPending && !statistics.reflectionActive && !statistics.reflectionFailed,
          "Reflection readiness is available and idle before an authored request");
  ok(api.Shutdown());
  std::puts("Bake statistics legacy prefix and reflection readiness passed");
}
