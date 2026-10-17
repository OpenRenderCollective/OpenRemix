#include "forward/ray/capture.h"
#include <algorithm>
#include <map>
#include <remix/remix_native_ext.h>
#include <tuple>

namespace remix::forward::ray {
  namespace {
    bool acceptsTransport(const scene::Draw &draw) {
      if (accepts(draw))
        return true;
      const bool coverage = bake::sourceOverCoverage(draw.blend);
      const bool boundary = draw.material->bakeSmoothBoundary && (!draw.blend.enabled || coverage);
      return draw.traced() && draw.blend.mask == 15 && (coverage || boundary || bake::modulatingDecal(draw)) &&
             (!draw.material->transmission || draw.material->bakeSmoothBoundary) && !draw.cutGroup &&
             !draw.wireframe && draw.depth && !draw.viewModel && !draw.spriteFlags &&
             (draw.material->normalEncoding != 5 || draw.material->bakeSmoothBoundary) &&
             (!draw.decal || draw.classification == REMIXAPI_INSTANCE_STATIC_EXT) &&
             (draw.depthCompare == 1 || draw.depthCompare == 3);
    }
  } // namespace
  Capture capture(const scene::BakeScene *authored, std::span<const scene::Draw> current,
                  std::span<const scene::SphereLight> lights, const scene::DistantLight &sun,
                  const scene::DomeLight &dome, float sceneScale, double seconds) {
    Capture result;
    result.source = std::make_shared<scene::BakeScene>();
    auto &source = *result.source;
    source.ready = true;
    source.sceneScale = sceneScale;
    const bool complete = authored && authored->ready;
    source.sun = sun.isDistant || !complete ? sun : authored->sun;
    source.dome = dome.enabled || !complete ? dome : authored->dome;
    std::map<uint64_t, scene::SphereLight> emitters;
    if (complete)
      for (const auto &light : authored->lights)
        emitters.insert_or_assign(light.id, light);
    for (const auto &light : lights)
      emitters.insert_or_assign(light.id, light);
    for (const auto &[id, light] : emitters)
      source.lights.push_back(light);
    using Key = std::tuple<uint64_t, uint32_t, uint32_t>;
    auto key = [](const scene::Draw &draw) {
      return Key{draw.instanceId, draw.instanceIndex, draw.surfaceIndex};
    };
    std::map<Key, scene::Draw> retained;
    if (complete)
      for (const auto &draw : authored->draws)
        if (acceptsTransport(draw))
          retained.insert_or_assign(key(draw), draw);
    for (const auto &draw : current) {
      if (!draw.instanceId) {
        if (acceptsTransport(draw))
          source.draws.push_back(draw);
      } else if (acceptsTransport(draw))
        retained.insert_or_assign(key(draw), draw);
      else
        retained.erase(key(draw));
    }
    for (const auto &[id, draw] : retained)
      source.draws.push_back(draw);
    result.inputs = bake::pack(source, seconds);
    uint64_t hash = 1469598103934665603ull;
    auto add = [&](const auto &value) { hash = scene::fastHash(&value, sizeof(value), hash); };
    auto array = [&](const auto &values) {
      add(values.size());
      if (!values.empty())
        hash = scene::fastHash(values.data(), values.size() * sizeof(values[0]), hash);
    };
    array(result.inputs.vertices);
    array(result.inputs.triangles);
    array(result.inputs.materials);
    add(sceneScale);
    // Analytic light values and environment transforms stream per frame; only
    // sampled texture membership/revisions belong to acceleration/atlas reuse.
    for (const auto &texture : result.inputs.textures) {
      add(reinterpret_cast<uintptr_t>(texture.image.get()));
      if (texture.image->updates) {
        std::lock_guard lock(texture.image->updates->lock);
        add(texture.image->updates->revision);
      }
    }
    result.key = hash;
    return result;
  }
} // namespace remix::forward::ray
