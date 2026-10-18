#pragma once
#include "forward/bake/grid.h"
#include "forward/lightmap/data.h"
#include "forward/probes/data.h"

namespace remix::forward::bake {
  // Optional worker-side phase receipts. The environment variable names a JSONL
  // output file; disabled receipts do no hashing. Diagnostic failure cannot
  // abort publication. Fields are hashed separately to locate changed data.
  void dumpPhaseFingerprint(const char *phase, uint64_t source, const Grid &grid, const lightmap::Layout &layout,
                            std::span<const Result> surfaces, std::span<const lightmap::Sun> sun,
                            std::span<const probes::Capture> captures) noexcept;
}
