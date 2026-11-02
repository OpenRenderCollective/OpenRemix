#pragma once
#include "forward/probes/data.h"

namespace remix::forward::probes {
// Immutable once shared: raw transport and filtered HDR data, never diffuse
// assets, a renderer, transport scene or GPU image ownership.
struct CapturePayload {
  std::vector<Capture> captures;
  Data data;
};
} // namespace remix::forward::probes
