#pragma once
#include "forward/lighting.h"
#include <set>
#include <stop_token>
namespace remix::forward::bake {
  struct ReceiverTileOptions {
    double tileMetres = 4, haloMetres = 0;
    uint32_t maximumTiles = 1u << 20;
    uint64_t maximumCandidates = 16u << 20;
  };
  struct ReceiverTileCoverage {
    double tileMetres = 0, haloMetres = 0;
    uint64_t occupied = 0, rectangle = 0;
  };
  // Conservative closed triangle/expanded-tile overlap in projected XY metres.
  // A halo is an L-infinity margin, not proof of all dynamic receiver coverage.
  class ReceiverTiles {
  public:
    explicit ReceiverTiles(ReceiverTileOptions options);
    void include(const std::array<Vec3, 3> &triangle, std::stop_token cancelled = {});
    ReceiverTileCoverage coverage() const;
  private:
    ReceiverTileOptions m_options;
    uint64_t m_candidates = 0;
    std::set<std::array<int32_t, 2>> m_tiles;
  };
}

