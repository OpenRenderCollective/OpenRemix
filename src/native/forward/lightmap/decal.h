#pragma once
#include "forward/lightmap/data.h"
namespace remix::forward::lightmap {
  // Clip captured static paint onto mapped receiver triangles. Reuse their UVs,
  // chart bounds and light ownership; never allocate decal lightmap charts.
  std::vector<Mesh> decalMeshes(const scene::BakeScene &source, std::span<const Mesh> receivers,
                                std::stop_token cancelled = {});
}
