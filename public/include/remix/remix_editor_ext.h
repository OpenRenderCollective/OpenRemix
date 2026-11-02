// Optional editor ABI (Linux, Windows); the Remix 0.1003 table is unchanged.
#pragma once
#include "remix_c.h"
#define REMIXAPI_STRUCT_TYPE_EDITOR_STARTUP_EXT ((remixapi_StructType)0x7fff1010)
typedef struct remixapi_EditorStartupEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t version; // 1
  remixapi_Bool headless;
  remixapi_Bool disableProfiles;
  remixapi_Bool disableMods;
} remixapi_EditorStartupEXT;
// Version 1 is the toolkit EditorSharedFrame contract. The caller owns all
// descriptors (Linux file descriptors, Windows NT handles).
// RGBA8 UNORM, optimal tiling, exclusive sharing, dedicated opaque-FD memory
// (opaque Win32 handles on Windows).
// Present waits for GPU completion before reporting success. Consumers wait on
// ready, acquire EXTERNAL -> their queue in GENERAL, then release to EXTERNAL
// in GENERAL and signal released. No second Present before the consumer releases.
typedef struct remixapi_EditorSharedFrame {
  uint32_t version, width, height, memory_type;
  uint64_t allocation_size;
  uint8_t device_uuid[16];
#if defined(_WIN32)
  void *memory_handle, *ready_handle, *released_handle;
#else
  int memory_fd, ready_fd, released_fd;
#endif
} remixapi_EditorSharedFrame;
// Version 2 (frame.version = 2; pass &frame) adds the scene's depth and may be
// configured again for a new size. Depth: R32 SFLOAT at the frame's size,
// optimal tiling, dedicated memory exported as the colour's, holding the depth
// the composite wrote (forward [0,1] of the camera's projection, 1 where
// nothing was hit; the previous frame's while the tracer rebuilds). Both images
// share ready/released and the layout rules above. Without a float depth target
// the depth descriptor is -1 (null on Windows) and depth_allocation_size 0.
// Configuring again releases the previous images and semaphores: the consumer
// must have finished with them.
typedef struct remixapi_EditorSharedFrame2 {
  remixapi_EditorSharedFrame frame;
  uint64_t depth_allocation_size;
  uint32_t depth_memory_type;
#if defined(_WIN32)
  void *depth_memory_handle;
#else
  int depth_memory_fd;
#endif
} remixapi_EditorSharedFrame2;
#ifdef __cplusplus
extern "C" {
#endif
REMIXAPI int REMIXAPI_CALL remixapi_EditorConfigureSharedFrame(remixapi_EditorSharedFrame *frame);
#ifdef __cplusplus
}
#endif
