#include "forward/gpu.h"
#include <utility>

namespace remix::forward {
  void Renderer::Gpu::prepareDepthBatch(std::shared_ptr<const bake::Volume> source,
                                        std::span<const DrawData> materials) {
    if (!depthBatchPipeline || !options.depthPrepass) return;
    auto &frame = frames[slot];
    if (!frame.depthBatch) frame.depthBatch = std::make_unique<depth_batch::Batch>(physical, device);
    // Borrow the storage by value so an exception also releases owners and
    // draw references from this preparation.
    auto sources = std::move(depthSourceScratch);
    auto draws = std::move(depthDrawScratch);
    const auto identity = scene::identity();
    for (auto &draw : frame.commands) {
      if (!draw.captured || !draw.opaqueDepth || draw.source.surface->bonesPerVertex != 0) continue;
      if (draw.index >= materials.size() || draw.index >= depth_batch::identityTransform) continue;
      const auto *mapped = draw.lightmap;
      if (mapped && !source) continue;
      const auto &material = materials[draw.index];
      const uint32_t tag = material.model == identity
          ? depth_batch::identityTransform | (material.state[3] & depth_batch::rejectBackfaces)
          : draw.index;
      sources.push_back({mapped ? mapped->vertices.get() : draw.geometry->vertices.get(),
          mapped ? mapped->indices.get() : draw.geometry->indices.get(),
          uint32_t(mapped ? sizeof(lightmap::Vertex) : sizeof(scene::Vertex)),
          mapped ? 0 : draw.source.firstIndex, mapped ? mapped->count : draw.count,
          mapped ? 0 : draw.source.vertexOffset, tag,
          draw.cameraVisible,
          mapped ? std::static_pointer_cast<const void>(source) :
                   std::static_pointer_cast<const void>(draw.geometry)});
      draws.push_back(&draw);
    }
    if (frame.depthBatch->prepare(sources))
      for (auto *draw : draws) draw->depthBatched = true;
    sources.clear();
    draws.clear();
    depthSourceScratch = std::move(sources);
    depthDrawScratch = std::move(draws);
  }
} // namespace remix::forward
