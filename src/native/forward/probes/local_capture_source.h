#pragma once
#include "forward/probes/image_asset.h"

namespace remix::forward::probes {
struct ReceiverCoveragePlan;
struct ReceiverCoverageEvidence;
struct CoverageReadyImage;
// Optional prepared local layers, not an active receiver selection. Publication
// retains only reflection assets/evidence; no diffuse volume is needed.
struct LocalCaptureSource {
  std::shared_ptr<ImageAsset> image;
  std::shared_ptr<const ReceiverCoveragePlan> plan;
  std::shared_ptr<const CoverageReadyImage> ready;
  std::shared_ptr<const ReceiverCoverageEvidence> evidence;
};
}
