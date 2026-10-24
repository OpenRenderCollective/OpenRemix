#include "forward/probes/coverage_context.h"
#include <bit>
#include <stdexcept>

namespace remix::forward::probes {
  CoveragePlanContext coveragePlanContext(const scene::BakeScene &source, const CoverageBaselineSet &baseline) {
    if (baseline.composed && (!baseline.authoredSceneId || !baseline.authoredRevision ||
                             baseline.authoredSceneId != source.sceneId))
      throw std::invalid_argument("Coverage baseline belongs to a different authored scene");
    CoveragePlanContext result;
    result.baseline = baseline;
    result.sceneId = source.sceneId;
    result.revision = source.revision;
    result.contentHash = source.contentHash;
    result.chartRevision = source.chartRevision;
    result.reflectionRevision = source.reflectionRevision;
    result.scaleBits = std::bit_cast<uint32_t>(source.sceneScale);
    result.ready = source.ready;
    result.valid = true;
    return result;
  }
  bool matchesCoverageContext(const CoveragePlanContext &context, const scene::BakeScene &source,
                              const CoverageBaselineSet &baseline) {
    if (!context.valid || context.sceneId != source.sceneId || context.revision != source.revision ||
        context.contentHash != source.contentHash || context.chartRevision != source.chartRevision ||
        context.reflectionRevision != source.reflectionRevision || context.ready != source.ready ||
        context.scaleBits != std::bit_cast<uint32_t>(source.sceneScale)) return false;
    const auto &old = context.baseline;
    if (old.composed != baseline.composed || old.disabled != baseline.disabled ||
        old.authoredSceneId != baseline.authoredSceneId || old.authoredRevision != baseline.authoredRevision ||
        old.parameters != baseline.parameters || old.volumes.size() != baseline.volumes.size()) return false;
    for (size_t i = 0; i < old.volumes.size(); ++i) {
      const auto &a = old.volumes[i], &b = baseline.volumes[i];
      if (a.protectedCapture != b.protectedCapture || a.source.automatic != b.source.automatic ||
          a.source.layer != b.source.layer ||
          std::bit_cast<uint32_t>(a.blendDistance) != std::bit_cast<uint32_t>(b.blendDistance) ||
          std::bit_cast<std::array<uint32_t, 32>>(a.record) !=
              std::bit_cast<std::array<uint32_t, 32>>(b.record)) return false;
    }
    return true;
  }
}
