#pragma once
#include <cstdint>

namespace remix::trace {
  // The slowest destruction of a buffer since this was last read (diagnostics):
  // its size, and whether it freed memory of its own; and the slowest freeing
  // of a memory block.
  inline double g_slowestBufferDestroy = 0, g_slowestBlockFree = 0;
  inline uint64_t g_slowestBufferDestroyBytes = 0;
  inline bool g_slowestBufferDestroyOwn = false;
  // Sparse rendering in the last traced frame (diagnostics): whether the
  // opaque pass was launched over the selected pixels, the share selected,
  // and otherwise why not (a string literal).
  struct SparseState {
    bool requested = false, launched = false;
    float rate = 1;
    const char *reason = "";
  };
  inline SparseState g_sparseState;
  // Ray portals (remixapi_MaterialInfoPortalEXT) in the last traced frame
  // (diagnostics): portal draws, pairs with both portals drawn, draws of an
  // index already taken, teleports per camera ray, and whether the pass that
  // traces them ran (raster primary with a denoiser).
  struct RayPortalState {
    uint32_t surfaces = 0, pairs = 0, duplicates = 0, teleports = 0;
    bool traced = false;
  };
  inline RayPortalState g_rayPortalState;
} // namespace remix::trace
