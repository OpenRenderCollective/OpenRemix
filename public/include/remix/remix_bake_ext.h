#pragma once
#include "remix_c.h"

#ifdef __cplusplus
extern "C" {
#endif

// Optional MaterialInfo chain: bake a smooth, constant optical boundary while
// normal-map ripples remain live. Only measured translucent bodies support this
// policy (index 1..4; measurement distance >= 1e-6 world units). It excludes
// normal textures/scrolling from capture, not animated base,
// emission, geometry or optical properties. The host must republish optical edits.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_BOUNDARY_EXT ((remixapi_StructType)0x7fff1040)
typedef struct remixapi_MaterialBakeBoundaryEXT {
  remixapi_StructType sType;
  void *pNext;
  remixapi_Bool smoothBoundary;
} remixapi_MaterialBakeBoundaryEXT;

#define REMIXAPI_BAKE_SCENE_VERSION_EXT 1u
#define REMIXAPI_BAKE_SCENE_LOADING_EXT 0u
#define REMIXAPI_BAKE_SCENE_READY_EXT 1u

// A complete, explicitly authored static scene, independent of camera draws
// and of the runtime's automatic persistent-static classification. Instances
// are handles from AddSceneInstancesEXT; lights are CreateLight handles.
// Include every static transport surface and only time-independent lights.
// Static emissive materials belong to their instances, not the light array.
//
// LOADING invalidates the previous scene and carries no instances or lights.
// READY atomically captures owned inputs. The caller must republish after
// changing included geometry, placement, materials, mods, textures or lights.
// A snapshot never follows later handle replacements or mutable caller arrays.
// Repeated requests reuse the snapshot only while its dependency revisions match.
// Replay retained instances after material/mod replacements before republishing.
// Animated/skinned/deforming inputs and dynamic textures are refused. Different
// handles may describe the same content; they do not form the cache identity.
// Null info unloads. Invalid requests leave the previous snapshot intact.
typedef struct remixapi_BakeSceneInfoEXT {
  uint32_t structSize, version;
  uint64_t sceneId, revision; // nonzero map/content identity, host input revision
  float sceneScale;           // metres per world unit, finite and positive
  uint32_t state;
  uint32_t instanceCount, lightCount;
  const uint64_t *instances;
  const remixapi_LightHandle *lights;
} remixapi_BakeSceneInfoEXT;

// Optional version 2 descriptor. Pass &info.scene to SetBakeSceneEXT, setting
// scene.structSize to sizeof(info) and scene.version to the value below. The
// original version-1 layout remains supported. Bounds select probe placement,
// never remove transport geometry (for example a distant 3D skybox).
#define REMIXAPI_BAKE_SCENE_BOUNDS_VERSION_EXT 2u
typedef struct remixapi_BakeSceneBoundsInfoEXT {
  remixapi_BakeSceneInfoEXT scene;
  float minimum[3], maximum[3];
} remixapi_BakeSceneBoundsInfoEXT;

// Optional version 3 surface charts. Each chart owns a non-overlapping triangle
// range of one retained instance replica/surface. texcoords contains indexCount
// pairs in index-corner order, in luxel units: (0,0) and (width-1,height-1) are
// the first and last texel centres. Separate corner coordinates preserve seams
// without changing core mesh vertices, and support displaced geometry.
// Arrays are copied on READY. Advance chartRevision after any chart edit;
// repeated requests with the same chartRevision promise identical chart data.
// Dimensions are 2..4096. Ranges are triangle-aligned and cannot overlap on
// the same draw. Uncharted geometry continues to use irradiance probes.
#define REMIXAPI_BAKE_SCENE_SURFACES_VERSION_EXT 3u
typedef struct remixapi_BakeSurfaceChartEXT {
  uint64_t instance;
  uint32_t surfaceIndex, instanceIndex;
  uint32_t firstIndex, indexCount, width, height;
  const float *texcoords;
} remixapi_BakeSurfaceChartEXT;
typedef struct remixapi_BakeSceneSurfacesInfoEXT {
  remixapi_BakeSceneBoundsInfoEXT bounds;
  uint64_t chartRevision;
  uint32_t chartCount, reserved;
  const remixapi_BakeSurfaceChartEXT *charts;
} remixapi_BakeSceneSurfacesInfoEXT;

// Optional version 4 reflection capture seeds. Positions are hints, never
// authored box boundaries: the runtime still validates and fits empty space.
// Ordered priority; at most 4096 xyz triples. Arrays are copied on READY.
// Advance reflectionRevision after an edit; hints can be independent of the
// irradiance grid (for example Source env_cubemap placements in small rooms).
#define REMIXAPI_BAKE_SCENE_REFLECTIONS_VERSION_EXT 4u
typedef struct remixapi_BakeSceneReflectionsInfoEXT {
  remixapi_BakeSceneSurfacesInfoEXT surfaces;
  uint64_t reflectionRevision;
  uint32_t positionCount, reserved;
  const float *positions;
} remixapi_BakeSceneReflectionsInfoEXT;

// Optional version 5 stationary controllable lights. authoredLight is a separate,
// immutable local-light intensity envelope; runtimeLight is the corresponding live light,
// including when off. Their emitter geometry must match. Never draw authoredLight.
// The runtime captures authored radiance/geometry with runtime emitter identities.
// Runtime colour/intensity edits within that envelope do not invalidate this capture;
// geometry or authored envelope edits do.
// Group identifiers are adapter-assigned 1..127; no engine lightstyle system is
// required. A single stationary on/off light can own a group. Members of an
// indirect group share a scalar current/authored intensity. The host supplies
// current light values; animation curves and game/entity logic stay in the host.
// Arrays are copied on READY; do not also include these handles in scene.lights.
// Forward+ bakes unit direct/static visibility and optionally separate grouped
// indirect surface/probe components. Shared base irradiance and frozen reflection
// captures exclude controllable energy. Reflection relighting remains pending.
#define REMIXAPI_BAKE_SCENE_STYLED_LIGHTS_VERSION_EXT 5u

typedef struct remixapi_BakeStyledLightEXT {
  remixapi_LightHandle authoredLight, runtimeLight;
  uint32_t style, reserved;
} remixapi_BakeStyledLightEXT;

typedef struct remixapi_BakeSceneStyledLightsInfoEXT {
  remixapi_BakeSceneReflectionsInfoEXT reflections;
  uint32_t styledLightCount, reserved;
  const remixapi_BakeStyledLightEXT *styledLights;
} remixapi_BakeSceneStyledLightsInfoEXT;

#define REMIXAPI_BAKE_WAITING_EXT 0u
#define REMIXAPI_BAKE_PREPARING_EXT 1u
#define REMIXAPI_BAKE_TRACING_EXT 2u
#define REMIXAPI_BAKE_READY_EXT 3u
#define REMIXAPI_BAKE_FAILED_EXT 4u
#define REMIXAPI_BAKE_UNSUPPORTED_EXT 5u
#define REMIXAPI_BAKE_DISABLED_EXT 6u
typedef struct remixapi_BakeStatisticsEXT {
  uint32_t structSize, state, supported, active;
  uint64_t requestedScene, requestedHash, activeHash;
  uint32_t probes, completed, samples, bounces;
  uint64_t memoryBytes;
  char status[192];
} remixapi_BakeStatisticsEXT;
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetBakeStatisticsEXT)(remixapi_BakeStatisticsEXT *out);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetBakeStatisticsEXT(remixapi_BakeStatisticsEXT *out);

typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetBakeSceneEXT)(const remixapi_BakeSceneInfoEXT *info);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetBakeSceneEXT(const remixapi_BakeSceneInfoEXT *info);

#ifdef __cplusplus
}
#endif
