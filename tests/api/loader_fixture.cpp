#include <remix/remix_c.h>

namespace {

  remixapi_ErrorCode REMIXAPI_CALL shutdown() { return REMIXAPI_ERROR_CODE_SUCCESS; }

} // namespace

extern "C" REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_InitializeLibrary(const remixapi_InitializeLibraryInfo *info, remixapi_Interface *outResult) {
  if (info == nullptr || outResult == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }

  const uint64_t expected =
      REMIXAPI_VERSION_MAKE(REMIXAPI_VERSION_MAJOR, REMIXAPI_VERSION_MINOR, REMIXAPI_VERSION_PATCH);
  if (info->version != expected) {
    return REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION;
  }

  *outResult = {};
  outResult->Shutdown = shutdown;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
