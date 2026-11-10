#pragma once
#include "forward/probes/image_asset.h"

namespace remix::forward::probes {
  // Publication boundary for automatic reflections. Readiness is explicit;
  // consumers retain only the image owner, never inspect diffuse bake storage.
  struct CaptureSource {
    uint64_t sceneHash = 0;
    std::shared_ptr<const ImageAsset> image;
  };
}
