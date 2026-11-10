#include "forward/shadows/moving_cache.h"

namespace remix::forward::shadows {
  namespace {
    template <class T> bool sameOwner(const std::weak_ptr<T> &a, const std::weak_ptr<T> &b) {
      return !a.owner_before(b) && !b.owner_before(a);
    }
    bool sameAlpha(const AlphaCoverage &a, const AlphaCoverage &b) {
      return a.textured == b.textured && (!a.textured ||
          sameOwner(a.texture, b.texture)) &&
          a.tint == b.tint && a.uv == b.uv && a.factor == b.factor && a.state == b.state &&
          a.decodeColor == b.decodeColor && a.sampler == b.sampler && a.samplerState == b.samplerState;
    }
    bool sameCaster(const MovingCaster &a, const MovingCaster &b) {
      // Scene submission validates the matrices before publishing the pose.
      // Reused immutable pose objects already prove identical matrices.
      return sameOwner(a.surface, b.surface) && a.model == b.model &&
          a.opaque == b.opaque && (a.opaque || sameAlpha(a.alpha, b.alpha)) &&
          a.firstIndex == b.firstIndex && a.indexCount == b.indexCount && a.vertexOffset == b.vertexOffset &&
          (a.bones == b.bones || (a.bones && b.bones && *a.bones == *b.bones));
    }
  }
  AlphaCoverage alphaCoverage(const DrawData &data, const scene::Material &material, uint64_t sampler) {
    AlphaCoverage result;
    const auto &texture = material.texture;
    // CPU revisions can lead uploaded GPU texels. Decline in-place updates;
    // immutable replacement objects are safely tracked by weak ownership.
    auto immutable = [](const auto &image) { return !image || (!image->dynamic && !image->updates); };
    result.cacheable = immutable(texture) && (!texture || immutable(texture->encodedSource));
    result.texture = texture;
    result.textured = bool(texture);
    result.tint = data.tint; result.uv = data.uv; result.factor = data.factor;
    result.state = data.state;
    result.decodeColor = (data.maps[0] & 32u) != 0;
    result.sampler = sampler;
    result.samplerState = {material.filter, material.wrapU, material.wrapV};
    return result;
  }
  void MovingCache::reset() {
    m_valid = false;
    m_casters.clear();
  }
  bool MovingCache::update(const View &view, std::span<const MovingCaster> casters) {
    // A cleared slot represents empty space for any light projection. Keep
    // it until a caster returns or the owner explicitly resets the slot.
    if (m_valid && m_casters.empty() && casters.empty()) return false;
    if (casters.size() > maximumCasters) { reset(); return true; }
    bool unchanged = m_valid && sameView(m_view, view) && m_view.matrices == view.matrices &&
                     m_casters.size() == casters.size();
    size_t poseMatrices = 0;
    for (size_t i = 0; i < casters.size(); ++i) {
      const auto &caster = casters[i];
      const size_t count = caster.bones ? caster.bones->size() : 0;
      if ((!caster.opaque && (!caster.alpha.cacheable ||
              (caster.alpha.textured && caster.alpha.texture.expired()))) ||
          caster.surface.expired() || count > maximumPoseMatrices - poseMatrices) {
        reset();
        return true;
      }
      poseMatrices += count;
      unchanged = unchanged && sameCaster(m_casters[i], caster);
    }
    if (unchanged) return false;
    m_valid = false;
    m_casters.assign(casters.begin(), casters.end());
    m_view = view;
    m_valid = true;
    return true;
  }
} // namespace remix::forward::shadows
