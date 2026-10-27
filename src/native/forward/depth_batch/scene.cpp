#include "forward/gpu.h"

namespace remix::forward {
  void Renderer::Gpu::prepareDepthBatch(std::shared_ptr<const bake::Volume> source) {
    if (!depthBatchPipeline || !options.depthPrepass) return;
    auto &frame = frames[slot];
    if (!frame.depthBatch) frame.depthBatch = std::make_unique<depth_batch::Batch>(physical, device);
    std::vector<depth_batch::Source> sources;
    std::vector<Command *> draws;
    for (auto &draw : frame.commands) {
      if (!draw.captured || !draw.opaqueDepth || draw.source.surface->bonesPerVertex != 0) continue;
      const auto *mapped = draw.lightmap;
      if (mapped && !source) continue;
      sources.push_back({mapped ? mapped->vertices.get() : draw.geometry->vertices.get(),
          mapped ? mapped->indices.get() : draw.geometry->indices.get(),
          uint32_t(mapped ? sizeof(lightmap::Vertex) : sizeof(scene::Vertex)),
          mapped ? 0 : draw.source.firstIndex, mapped ? mapped->count : draw.count,
          mapped ? 0 : draw.source.vertexOffset, draw.index,
          draw.source.rasterVisible && intersectsClip(draw.worldBounds, frame.data.camera),
          mapped ? std::static_pointer_cast<const void>(source) :
                   std::static_pointer_cast<const void>(draw.geometry)});
      draws.push_back(&draw);
    }
    if (frame.depthBatch->prepare(sources))
      for (auto *draw : draws) draw->depthBatched = true;
  }
} // namespace remix::forward
