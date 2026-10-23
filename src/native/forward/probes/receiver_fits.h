#pragma once
#include "forward/probes/regions.h"
#include "forward/probes/receiver_site_checks.h"

namespace remix::forward::probes {
  struct ReceiverProjection {
    Bounds projection, influence;
    bool coversReceiver = false;
  };
  struct ReceiverSiteFit {
    uint32_t site = 0;
    std::vector<ReceiverProjection> variants;
  };
  struct ReceiverFits {
    bool available = false;
    uint32_t supported = 0, partitionRefused = 0, duplicates = 0;
    std::vector<ReceiverSiteFit> attempts;
  };
  // Bounded comparison with the existing strict occupancy fitter. Supported
  // hardware samples do not override partition walls. No capture is allocated,
  // and the resulting proxies still require an actual-geometry assessment.
  ReceiverFits fitReceiverSites(const scene::BakeScene &source,
      std::span<const ReceiverSample> receivers, const ReceiverSiteAudit &admission,
      const ReceiverSiteChecks &checks, uint32_t budget = 32, std::stop_token cancelled = {});
} // namespace remix::forward::probes
