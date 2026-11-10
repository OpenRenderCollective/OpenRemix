#pragma once
#include "scene/types.h"

namespace remix::forward::bake {
  // Both exact bindings and derived receiver mapping require these identities.
  // This only selects candidates. The callers still verify the full draw.
  inline uint64_t drawLookupKey(const scene::Draw &draw) {
    const uint64_t subdraw = (uint64_t(draw.instanceIndex) << 32) | draw.surfaceIndex;
    return scene::fastHash(&subdraw, sizeof(subdraw), draw.instanceId);
  }
} // namespace remix::forward::bake
