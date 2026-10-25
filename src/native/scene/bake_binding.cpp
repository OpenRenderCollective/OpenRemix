#include "scene/bake_scene.h"
#include <remix/remix_native_ext.h>

namespace remix::scene {
  namespace {
    bool sameBlend(const Blend &a, const Blend &b) {
      return a.enabled == b.enabled && a.src == b.src && a.dst == b.dst && a.op == b.op &&
             a.srcAlpha == b.srcAlpha && a.dstAlpha == b.dstAlpha && a.alphaOp == b.alphaOp &&
             a.mask == b.mask && a.colorOp == b.colorOp && a.color1 == b.color1 && a.color2 == b.color2 &&
             a.alphaOpTexture == b.alphaOpTexture && a.alpha1 == b.alpha1 && a.alpha2 == b.alpha2 &&
             a.factor == b.factor;
    }
  } // namespace
  bool matchesBakeDraw(const BakeScene &scene, uint32_t index, const Draw &draw) {
    if (draw.classification != REMIXAPI_INSTANCE_STATIC_EXT || index >= scene.draws.size() ||
        index >= scene.bindings.size())
      return false;
    const auto &expected = scene.draws[index];
    const auto &binding = scene.bindings[index];
    return draw.instanceId == expected.instanceId && draw.instanceIndex == expected.instanceIndex &&
           draw.surfaceIndex == expected.surfaceIndex && draw.surface == binding.surface.lock() &&
           (scene.receiverOnly(index) || draw.material == binding.material.lock()) && draw.model == expected.model &&
           draw.firstIndex == expected.firstIndex && draw.indexCount == expected.indexCount &&
           draw.vertexOffset == expected.vertexOffset && draw.doubleSided == expected.doubleSided &&
           draw.reflectionOnly == expected.reflectionOnly &&
           draw.ignoreAlpha == expected.ignoreAlpha && draw.terrain == expected.terrain &&
           draw.alphaOverride == expected.alphaOverride &&
           draw.compare == expected.compare && draw.reference == expected.reference &&
           sameBlend(draw.blend, expected.blend);
  }
} // namespace remix::scene
