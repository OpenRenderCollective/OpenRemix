#pragma once
#include "forward/bake/probe_receiver_report.h"

namespace remix::forward::bake {
  // Diagnostic opaque segment evidence shared by pixel and anchor reports.
  struct ConnectionAudit {
    std::array<Vec3, 16> positions{};
    std::array<bool, 16> tested{};
    std::array<std::optional<ReceiverHit>, 16> blockers{};
  };
  ConnectionAudit probeConnections(const scene::BakeScene &source, const Grid &grid,
      const ProbeStencil &stencil, std::span<const Grid::Origin> origins,
      std::stop_token cancelled, uint32_t excludedDraw = UINT32_MAX);
}
