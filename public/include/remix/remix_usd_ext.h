// Independent versioned USD ABI. No additions to the Remix 0.1003 interface.
#pragma once
#include "remix_c.h"
typedef uint64_t remixapi_USDSceneHandleEXT;
typedef struct remixapi_USDLoadOptionsEXT {
  uint32_t structSize, version; // sizeof, 1
  const char *path;             // UTF-8 capture/project path
  double hostMetersPerUnit;     // 0: retain capture metadata
  uint32_t hostUpAxis;          // 0: retain metadata, 1: Y, 2: Z
  uint32_t reserved;
} remixapi_USDLoadOptionsEXT;
typedef struct remixapi_USDDrawOptionsEXT {
  uint32_t structSize, version; // sizeof, 1
  remixapi_Bool useStageCamera;
  float aspectRatio;
} remixapi_USDDrawOptionsEXT;
typedef struct remixapi_USDDiagnosticsEXT {
  uint32_t structSize, version;
  uint64_t meshes, materials, lights, cameras, generation;
  double metersPerUnit;
  uint32_t upAxis, warnings;
  char message[1024];
} remixapi_USDDiagnosticsEXT;
typedef struct remixapi_USDModsOptionsEXT {
  uint32_t structSize, version;
  const char *directory;
  double hostMetersPerUnit;
  uint32_t hostUpAxis;
  remixapi_Bool watch;
} remixapi_USDModsOptionsEXT;
#ifdef __cplusplus
extern "C" {
#endif
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ConfigureUSDModsEXT(const remixapi_USDModsOptionsEXT *);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetUSDModsDiagnosticEXT(char *, uint32_t);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_LoadUSDSceneEXT(const remixapi_USDLoadOptionsEXT *,
                                                                   remixapi_USDSceneHandleEXT *);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawUSDSceneEXT(remixapi_USDSceneHandleEXT,
                                                                   const remixapi_USDDrawOptionsEXT *);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ReloadUSDSceneEXT(remixapi_USDSceneHandleEXT);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyUSDSceneEXT(remixapi_USDSceneHandleEXT);
// Handle 0 returns the last load failure on the calling thread.
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetUSDDiagnosticsEXT(remixapi_USDSceneHandleEXT,
                                                                        remixapi_USDDiagnosticsEXT *);
#ifdef __cplusplus
}
#endif
