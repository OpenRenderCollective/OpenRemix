// Optional engine-agnostic reflection authoring ABI. The Remix 0.1003 table is unchanged.
#pragma once
#include "remix_c.h"
#define REMIXAPI_REFLECTION_VOLUMES_VERSION_EXT 1
#define REMIXAPI_REFLECTION_VOLUMES_SAMPLING_VERSION_EXT 2
#define REMIXAPI_REFLECTION_VOLUMES_MAX_EXT 64

typedef struct remixapi_ReflectionVolumeEXT {
  uint64_t id; // nonzero, stable within this scene
  float minimum[3], maximum[3]; // local projection bounds; version 1 also uses them for influence
  float worldToLocal[3][4]; // rows of a rigid transform; no scaling or shear
  float capture[3]; // world-space origin, independent of volume center
  int32_t priority; // -32768..32767; larger values win interiors and feather toward lower priorities at edges
} remixapi_ReflectionVolumeEXT;
typedef struct remixapi_ReflectionVolumesInfoEXT {
  uint32_t structSize, version;
  uint64_t sceneId, revision; // sceneId matches SetBakeSceneEXT; revision is scheduling metadata
  uint32_t volumeCount, reserved; // reserved must be zero
  const remixapi_ReflectionVolumeEXT *volumes;
} remixapi_ReflectionVolumesInfoEXT;

typedef struct remixapi_ReflectionSamplingEXT {
  float minimum[3], maximum[3]; // local receiver influence, independent of projection
  float blendDistance; // scene units; zero derives a 20%-of-half-extent edge band
} remixapi_ReflectionSamplingEXT;

// Pass &request.base to SetReflectionVolumesEXT. Original version-1 requests,
// volume records and sidecars retain their layouts and blending behavior.
typedef struct remixapi_ReflectionVolumesSamplingInfoEXT {
  remixapi_ReflectionVolumesInfoEXT base; // version = SAMPLING_VERSION, structSize = sizeof(this)
  const remixapi_ReflectionSamplingEXT *sampling; // volumeCount records, copied with volumes
} remixapi_ReflectionVolumesSamplingInfoEXT;
#ifdef __cplusplus
extern "C" {
#endif
// Copies caller data. NULL restores automatic placement. A non-NULL zero-count
// request explicitly disables local cubemaps. Updates never change diffuse bake inputs.
// Requests are scoped to sceneId and remain dormant while another scene is active.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetReflectionVolumesEXT)(
    const remixapi_ReflectionVolumesInfoEXT *info);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetReflectionVolumesEXT(
    const remixapi_ReflectionVolumesInfoEXT *info);
#ifdef __cplusplus
}
#endif
