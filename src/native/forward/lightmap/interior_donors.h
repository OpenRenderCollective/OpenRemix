#pragma once
#include "forward/lightmap/layout.h"

namespace remix::forward::lightmap {
  struct InteriorDonors {
    uint32_t hiddenSamples = 0, repairedTexels = 0, unresolvedCharts = 0;
  };
  // Closed static geometry can contain samples of an otherwise visible chart.
  // Keep their bright interior lighting out of filtering support. Only donor
  // indices change: coordinates, sample identities and chart light owners stay
  // intact. Charts without an exterior donor retain their original data.
  InteriorDonors repairInteriorDonors(Layout &layout, std::span<const bake::Result> results,
                                     std::stop_token cancelled = {});
} // namespace remix::forward::lightmap
