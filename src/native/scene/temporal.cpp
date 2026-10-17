#include "scene/temporal.h"

namespace remix::scene {
  void TemporalHistory::begin(TemporalEpoch epoch, uint64_t integrator, uint32_t width, uint32_t height) {
    m_pending.clear();
    m_nextEpoch = epoch;
    m_nextIntegrator = integrator;
    m_nextWidth = width;
    m_nextHeight = height;
    m_reset =
        !m_frames || epoch != m_epoch || integrator != m_integrator || width != m_width || height != m_height;
  }
  uint64_t TemporalHistory::materialSignature(const Draw &draw) {
    const auto &m = *draw.material;
    uint64_t hash = m.texture->fingerprint();
    auto add = [&](const auto &v) { hash = hashBytes(&v, sizeof(v), hash); };
    add(m.tint);
    add(m.emission);
    add(m.suppressEmissionLight);
    add(m.selfIllumination);
    add(m.selfIlluminationScale);
    add(m.roughness);
    add(m.metallic);
    add(m.pbr);
    add(m.normalEncoding);
    for (const auto &r : m.remap) {
      add(r.channel);
      add(r.curve);
      add(r.invert);
      add(r.exponent);
      add(r.bias);
      add(r.scale);
    }
    add(m.scrollLayers);
    add(m.scroll);
    add(m.refractiveIndex);
    add(m.transmittanceDistance);
    if (m.roughnessTexture)
      add(m.roughnessTexture->fingerprint());
    if (m.metallicTexture)
      add(m.metallicTexture->fingerprint());
    if (m.normalTexture)
      add(m.normalTexture->fingerprint());
    if (m.emissionTexture)
      add(m.emissionTexture->fingerprint());
    add(m.compare);
    add(m.reference);
    add(m.drawAlpha);
    add(m.translucent);
    add(m.transmission);
    add(m.filter);
    add(m.wrapU);
    add(m.wrapV);
    add(m.rows);
    add(m.columns);
    add(m.fps);
    const auto &b = draw.blend;
    add(b.enabled);
    add(b.mask);
    add(b.colorOp);
    add(b.color1);
    add(b.color2);
    add(b.alphaOpTexture);
    add(b.alpha1);
    add(b.alpha2);
    add(b.factor);
    add(draw.ignoreAlpha);
    add(draw.decal);
    add(draw.terrain);
    add(draw.alphaOverride);
    add(draw.compare);
    add(draw.reference);
    add(draw.doubleSided);
    return hash;
  }
  const TemporalHistory::SurfaceHistory *TemporalHistory::previous(const Draw &draw) const {
    // Materials and textures are immutable once shared, so the stored draw's
    // signature equals the one it had when it was staged.
    const auto *p = previousGeometry(draw);
    return p && materialSignature(p->draw) == materialSignature(draw) ? p : nullptr;
  }
  const TemporalHistory::SurfaceHistory *TemporalHistory::previousGeometry(const Draw &draw,
                                                                           uint64_t instanceId) const {
    if (m_reset || !instanceId)
      return nullptr;
    const TemporalKey key{instanceId, draw.instanceIndex, draw.surfaceIndex};
    const auto *found = m_previous.find(key);
    if (!found && m_fallback)
      found = m_fallback->previous(key);
    if (!found)
      return nullptr;
    const auto &p = *found;
    // Retained meshes preserve vertex identity, including changed bones. Streamed
    // replacements need both an explicit correspondence guarantee and exact
    // topology lineage; an update/storage hint alone is insufficient.
    const bool retained = p.draw.mesh == draw.mesh && p.draw.surface == draw.surface;
    const bool corresponding = draw.mesh && p.draw.mesh && draw.mesh->motionCorrespondence &&
                               p.draw.mesh->motionCorrespondence && draw.mesh->updateStream &&
                               draw.mesh->updateStream == p.draw.mesh->updateStream;
    if ((!retained && !corresponding) || p.draw.classification != draw.classification)
      return nullptr;
    return &p;
  }
  void TemporalHistory::stage(Draw &&draw, uint32_t textureFrame) {
    const uint64_t instanceId = draw.instanceId;
    stage(std::move(draw), instanceId, textureFrame);
  }
  void TemporalHistory::stage(Draw &&draw, uint64_t instanceId, uint32_t textureFrame) {
    if (!instanceId)
      return;
    const TemporalKey key{instanceId, draw.instanceIndex, draw.surfaceIndex};
    draw.instanceId = instanceId;
    m_pending.emplace(key, std::move(draw), textureFrame);
  }
  void TemporalHistory::commit() {
    // Keep both tables' storage: the old history is released here and the
    // emptied table is refilled by the next frame's stage() calls.
    std::swap(m_previous, m_pending);
    m_pending.clear();
    m_epoch = m_nextEpoch;
    m_integrator = m_nextIntegrator;
    m_width = m_nextWidth;
    m_height = m_nextHeight;
    ++m_frames;
  }
  void TemporalHistory::clear() {
    m_previous.clear();
    m_pending.clear();
    m_frames = 0;
    m_reset = true;
  }
} // namespace remix::scene
