#include "upscale/ffx_library.h"
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <windows.h>

namespace remix::upscale::ffx {
namespace {
std::filesystem::path moduleDirectory() {
  HMODULE module{};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&moduleDirectory), &module))
    throw std::runtime_error("FidelityFX: module path unavailable");
  std::wstring path(32768, L'\0');
  const auto size = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
  if (!size || size == path.size())
    throw std::runtime_error("FidelityFX: module path unavailable");
  path.resize(size);
  return std::filesystem::path(path).parent_path();
}
constexpr const wchar_t *loaderName = L"amd_fidelityfx_loader_vk.dll";
Api load() {
  const auto directory = moduleDirectory();
  HMODULE module{};
  for (const auto &candidate : {directory / "fidelityfx", directory / "upscale", directory}) {
    std::error_code error;
    const auto path = candidate / loaderName;
    if (!std::filesystem::is_regular_file(path, error))
      continue;
    // Its dependencies, and the effect DLLs it loads, from its own folder.
    module = LoadLibraryExW(path.c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module)
      throw std::runtime_error("FidelityFX: amd_fidelityfx_loader_vk.dll could not be loaded (error " +
                               std::to_string(GetLastError()) + ")");
    break;
  }
  if (!module)
    throw std::runtime_error("FidelityFX: amd_fidelityfx_loader_vk.dll not packaged");
  Api api;
  api.createContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
  api.destroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
  api.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module, "ffxConfigure"));
  api.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
  api.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
  if (!api.createContext || !api.destroyContext || !api.configure || !api.query || !api.dispatch)
    throw std::runtime_error("FidelityFX: amd_fidelityfx_loader_vk.dll lacks the ffx-api entry points");
  return api;
}
} // namespace

const Api &api() {
  // A failure is remembered: the DLLs do not appear while the process runs.
  static std::once_flag once;
  static Api loaded;
  static std::string failure;
  std::call_once(once, [] {
    try {
      loaded = load();
    } catch (const std::exception &e) {
      failure = e.what();
    }
  });
  if (!failure.empty())
    throw std::runtime_error(failure);
  return loaded;
}

void check(ffxReturnCode_t result, const char *what) {
  if (result != FFX_API_RETURN_OK)
    throw std::runtime_error(std::string(what) + " failed (ffx-api " + std::to_string(result) + ")");
}

std::string provider(ffxContext &context, const char *effect) {
  ffxQueryGetProviderVersion version{};
  version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  if (api().query(&context, &version.header) != FFX_API_RETURN_OK || !version.versionName)
    return effect;
  // A bare version number names the effect's family; anything else is a name.
  const std::string name = version.versionName;
  return !name.empty() && (name[0] < '0' || name[0] > '9') ? name : std::string(effect) + " " + name;
}

ffxApiHeader *versionOverride(uint64_t createDescType, void *device, ffxOverrideVersion &storage) {
  const char *wanted = std::getenv("REMIX_FSR_VERSION");
  if (!wanted || !*wanted)
    return nullptr;
  uint64_t count = 0;
  ffxQueryDescGetVersions query{};
  query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  query.createDescType = createDescType;
  query.device = device;
  query.outputCount = &count;
  check(api().query(nullptr, &query.header), "FSR version query");
  std::vector<uint64_t> ids(count);
  std::vector<const char *> names(count);
  query.versionIds = ids.data();
  query.versionNames = names.data();
  check(api().query(nullptr, &query.header), "FSR version query");
  std::string available;
  for (uint64_t i = 0; i < count; ++i) {
    const std::string name = names[i] ? names[i] : "";
    if (name.rfind(wanted, 0) == 0) {
      storage = {};
      storage.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
      storage.versionId = ids[i];
      return &storage.header;
    }
    available += (available.empty() ? "" : ", ") + name;
  }
  throw std::runtime_error(std::string("REMIX_FSR_VERSION=") + wanted + ": no such FSR provider (available: " +
                           available + ")");
}
} // namespace remix::upscale::ffx
