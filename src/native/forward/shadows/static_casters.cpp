#include "forward/shadows/static_casters.h"
#include "forward/lighting.h"

namespace remix::forward::shadows {
  namespace {
    bool staticCaster(const scene::Draw &draw) {
      return draw.classification == REMIXAPI_INSTANCE_STATIC_EXT && accepts(draw);
    }
  } // namespace
  uint64_t staticCasterKey(std::span<const scene::Draw> draws) {
    uint64_t key = 1469598103934665603ull;
    auto add = [&](const void *data, size_t bytes) { key = scene::fastHash(data, bytes, key); };
    bool any = false;
    for (const auto &draw : draws) {
      if (!staticCaster(draw)) continue;
      const uintptr_t owners[] = {reinterpret_cast<uintptr_t>(draw.surface.get()),
                                  reinterpret_cast<uintptr_t>(draw.material.get())};
      add(owners, sizeof(owners));
      add(draw.model.data(), sizeof(draw.model));
      const uint64_t identity[] = {draw.instanceId, draw.instanceIndex, draw.surfaceIndex, draw.firstIndex,
                                   draw.indexCount, uint64_t(uint32_t(draw.vertexOffset)),
                                   uint64_t(draw.doubleSided) | uint64_t(draw.terrain) << 1};
      add(identity, sizeof(identity));
      any = true;
    }
    return any ? key | 1 : 0;
  }
  std::shared_ptr<const scene::BakeScene> implicitStaticScene(std::span<const scene::Draw> draws,
                                                              float sceneScale) {
    auto result = std::make_shared<scene::BakeScene>();
    result->ready = true;
    result->sceneScale = sceneScale;
    for (const auto &draw : draws) {
      if (!staticCaster(draw)) continue;
      result->draws.push_back(draw);
      result->bindings.push_back({draw.surface, draw.material});
    }
    return result;
  }
} // namespace remix::forward::shadows
