// Optional reconstruction diagnostics. No change to the retained API table or layouts.
#pragma once
#include "remix_c.h"

// requestedMode: 0 off, 1 auto, 2 FSR 3.1, 3 DLSS, 4 XeSS (Windows).
// activeBackend: 0 native, 1 FSR 3.1, 2 DLSS SR, 3 DLSS Ray Reconstruction, 4 XeSS SR.
// Metal builds: 1 MetalFX temporal scaler, 3 MetalFX temporal denoised scaler.
// capabilities: bit 0 SDK built, bit 1 SR available, bit 2 RR available.
// structSize is caller capacity (at least 8); only the known prefix is written.
// Returned structSize is this implementation's size; version is 1.
typedef struct remixapi_ReconstructionDiagnosticsEXT {
  uint32_t structSize;
  uint32_t version;
  uint32_t capabilities;
  uint32_t requestedMode;
  uint32_t activeBackend;
  uint32_t renderWidth, renderHeight;
  uint32_t displayWidth, displayHeight;
  uint32_t reserved;
  uint64_t evaluatedFrames, historyResets, failures;
  uint64_t memoryBytes; // Reconstruction-owned images plus SDK-reported allocations.
  float gpuMilliseconds;
  char fallbackReason[256];
} remixapi_ReconstructionDiagnosticsEXT;

#ifdef __cplusplus
extern "C" {
#endif
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetReconstructionDiagnosticsEXT(remixapi_ReconstructionDiagnosticsEXT *out);
#ifdef __cplusplus
}
#endif
