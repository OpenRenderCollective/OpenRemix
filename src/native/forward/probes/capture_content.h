#pragma once
#include "forward/probes/data.h"

namespace remix::forward::probes {
struct CaptureContent {
  uint64_t filtered = 0, radiance = 0, inputs = 0;
  bool operator==(const CaptureContent &) const = default;
};
// Worker-only diagnostics; matches legacy serialized-image/RGB fingerprints
// without allocating a joined image or retaining capture arrays.
CaptureContent captureContent(std::span<const Capture> captures, const Data &data,
                              uint64_t inputs, std::stop_token cancelled = {});
}
