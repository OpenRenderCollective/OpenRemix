#pragma once
#include "forward/probes/capture_content.h"
#include "forward/probes/capture_source.h"
#include "forward/probes/local_capture_source.h"
#include "forward/probes/placement_seeds.h"

namespace remix::forward::probes {
  // Render-thread retained capture state. Images and admission are independent
  // of diffuse GPU data; jobs/submissions retain their own immutable inputs.
  // The preset scheduler controls publication and transport admission.
  struct CapturePublication {
    std::shared_ptr<ImageAsset> image;
    std::shared_ptr<const PlacementSeeds> seeds;
    std::shared_ptr<const LocalCaptureSource> local;
    uint64_t sceneHash = 0;
    CaptureContent content;

    CaptureSource source() const { return {sceneHash, image}; }
    const Header *header() const { return image ? &image->header() : nullptr; }
    // The caller supplies a fallback when no image has published yet.
    Descriptors descriptor(const ImageAsset *fallback, ReferenceProvenance receiver) const {
      return image ? image->descriptors(receiver) : fallback->descriptors();
    }
  };
}
