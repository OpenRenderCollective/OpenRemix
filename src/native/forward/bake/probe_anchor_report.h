#pragma once
#include "forward/bake/probe_receiver_report.h"
#include <iosfwd>

namespace remix::forward::bake {
  // Bounded geometry-anchor diagnostics; never used by normal reconstruction.
  void dumpProbeAnchorReport(std::ostream &out, const scene::BakeScene &source, const Grid &grid,
      std::span<const Grid::Origin> origins, std::span<const std::optional<ReceiverHit>> hits,
      std::stop_token cancelled);
}
