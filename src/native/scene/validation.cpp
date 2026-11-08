#include "scene/validation.h"
#include <cmath>
#include <cstring>

namespace remix::scene::detail {
  scene::Blend blend(const remixapi_InstanceInfoBlendEXT *info) {
    scene::Blend out;
    if (!info) {
      return out;
    }
    out.enabled = info->alphaBlendEnabled != 0;
    if (out.enabled) {
      if (info->srcColorBlendFactor > 14 || info->dstColorBlendFactor > 14 || info->colorBlendOp > 4 ||
          info->srcAlphaBlendFactor > 14 || info->dstAlphaBlendFactor > 14 || info->alphaBlendOp > 4 ||
          (info->writeMask & ~15u)) {
        throw unavailable;
      }
      out.src = info->srcColorBlendFactor;
      out.dst = info->dstColorBlendFactor;
      out.op = info->colorBlendOp;
      out.srcAlpha = info->srcAlphaBlendFactor;
      out.dstAlpha = info->dstAlphaBlendFactor;
      out.alphaOp = info->alphaBlendOp;
      out.mask = info->writeMask ? info->writeMask : 15;
    }
    // Zero-initialized texture state retains the default texture/vertex modulation.
    if (info->textureColorOperation) {
      out.colorOp = info->textureColorOperation;
      out.color1 = info->textureColorArg1Source;
      out.color2 = info->textureColorArg2Source;
    }
    if (info->textureAlphaOperation) {
      out.alphaOpTexture = info->textureAlphaOperation;
      out.alpha1 = info->textureAlphaArg1Source;
      out.alpha2 = info->textureAlphaArg2Source;
    }
    if (out.colorOp > 7 || out.alphaOpTexture > 7 || out.color1 > 3 || out.color2 > 3 || out.alpha1 > 3 ||
        out.alpha2 > 3) {
      throw unavailable;
    }
    out.factor = info->tFactor;
    return out;
  }
} // namespace remix::scene::detail

namespace remix::scene {
  using namespace detail;
  const void *extension(const void *next, remixapi_StructType type) {
    struct Header {
      remixapi_StructType type;
      const void *next;
    };
    const void *found = nullptr;
    for (unsigned i = 0; next; ++i) {
      if (i == 64) {
        throw invalid;
      }
      Header head;
      std::memcpy(&head, next, sizeof(head));
      if (head.type == type) {
        if (found) {
          throw invalid;
        }
        found = next;
      }
      next = head.next;
    }
    return found;
  }
} // namespace remix::scene
