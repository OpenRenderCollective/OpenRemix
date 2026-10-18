#pragma once
// Host passes (remixapi_HostPassEXT, runtime/host_passes.h) on Metal: the
// host's screen-space passes are SPIR-V shaders for the Vulkan pipeline, which
// the Metal backend does not replay yet. The interface is the Vulkan one, so
// the API layer is shared; a host asking is told the passes are off.
#include <remix/remix_native_ext.h>
#include <cstdint>

namespace remix {
  class HostPasses {
  public:
    remixapi_ErrorCode registerShader(const remixapi_HostShaderInfoEXT &, remixapi_HostShaderHandleEXT &out) {
      out = 0;
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
    remixapi_ErrorCode destroyShader(remixapi_HostShaderHandleEXT) { return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE; }
    remixapi_ErrorCode submit(const remixapi_HostPassListEXT &) { return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE; }
    remixapi_ErrorCode updateTarget(remixapi_HostTargetHandleEXT, uint32_t, uint32_t, uint32_t, const void *, uint64_t,
                                    uint32_t = 1) {
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
    bool pending() const { return false; }
    void setEnabled(bool) {}
    // remixapi_GetHostPassesEnabledEXT: never on Metal (whatever the setting).
    bool enabled() {
      m_asked = true;
      return false;
    }
    bool asked() const { return m_asked; }
    remixapi_HostPassStatisticsEXT statistics() const {
      return {.structSize = sizeof(remixapi_HostPassStatisticsEXT)};
    }

  private:
    bool m_asked = false;
  };
} // namespace remix
