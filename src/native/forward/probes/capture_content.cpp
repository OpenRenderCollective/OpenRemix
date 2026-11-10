#include "forward/probes/capture_content.h"
#include "scene/hash_stream.h"
#include <algorithm>
#include <stdexcept>

namespace remix::forward::probes {
CaptureContent captureContent(std::span<const Capture> captures, const Data &data,
                              uint64_t inputs, std::stop_token cancelled) {
  auto check = [&] {
    if (cancelled.stop_requested()) throw std::runtime_error("Reflection content hashing superseded");
  };
  check();
  CaptureContent result;
  if (!captures.empty()) {
    result.inputs = inputs;
    uint64_t hash = 1469598103934665603ull;
    for (const auto &capture : captures)
      for (size_t i = 0; i < capture.radiance.size(); ++i) {
        if ((i & 4095u) == 0) check();
        hash = scene::hashBytes(capture.radiance[i].data(), 3 * sizeof(float), hash);
      }
    result.radiance = hash | 1;
  }
  scene::HashStream filtered;
  filtered.append(&data.header, sizeof(data.header));
  for (size_t first = 0; first < data.texels.size();) {
    check();
    const auto count = std::min(data.texels.size() - first, size_t((1u << 20) / sizeof(Vec4)));
    filtered.append(data.texels.data() + first, count * sizeof(Vec4));
    first += count;
  }
  result.filtered = filtered.digest() | 1;
  return result;
}
}
