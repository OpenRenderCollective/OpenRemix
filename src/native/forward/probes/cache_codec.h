#pragma once
#include "forward/cache_io.h"
#include "forward/probes/data.h"

namespace remix::forward::probes::cache_codec {
  struct Limits {
    uint32_t count = 0, resolution = 0;
    uint32_t imageSize = 0, imageLayers = 0, storageRange = 0;
  };
  // Raw capture and filtered image payload only: no diffuse grid, atlas or
  // renderer state. Existing 32-record disk blocks remain byte-compatible.
  void read(cache_io::Reader &input, std::vector<Capture> &captures, Data &data, Limits limits);
  void validate(std::span<const Capture> captures, const Data &data);
  cache_io::Section fingerprint(std::span<const Capture> captures, const Data &data,
                                  std::stop_token cancelled = {});
  void write(cache_io::Writer &output, std::span<const Capture> captures, const Data &data);
}
