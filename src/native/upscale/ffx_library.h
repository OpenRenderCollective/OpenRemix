#pragma once
#include <ffx_api.h>
#include <string>

namespace remix::upscale::ffx {
// AMD's signed ffx-api loader (amd_fidelityfx_loader_vk.dll, FidelityFX SDK
// 2.4.0), loaded once per process and never unloaded. It loads the effect
// DLLs (amd_fidelityfx_upscaler_vk.dll, amd_fidelityfx_framegeneration_vk.dll)
// from its own folder: fidelityfx/ beside remix.dll in a package, upscale/ in
// the Meson build tree.
struct Api {
  PfnFfxCreateContext createContext{};
  PfnFfxDestroyContext destroyContext{};
  PfnFfxConfigure configure{};
  PfnFfxQuery query{};
  PfnFfxDispatch dispatch{};
};
// Throws std::runtime_error saying why the loader is unavailable.
const Api &api();
// Throws std::runtime_error "<what> failed (ffx-api <code>)".
void check(ffxReturnCode_t result, const char *what);
// The name of the provider a context was created with, e.g. "FSR 4.1.2".
std::string provider(ffxContext &context, const char *effect);
// Test switch (REMIX_FSR_VERSION=<version prefix>, e.g. 3): an override to
// chain into the creation description of `createDescType`, selecting the
// provider whose version starts with the prefix; null without the variable.
// `device`: the VkDeviceContext. Throws, listing the providers, when none
// matches.
ffxApiHeader *versionOverride(uint64_t createDescType, void *device, ffxOverrideVersion &storage);
} // namespace remix::upscale::ffx
