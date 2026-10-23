#pragma once
#include "forward/probes/evaluation.h"
#include "forward/probes/receiver_site_checks.h"

namespace remix::forward::probes {
  struct ReceiverProjection {
    Bounds projection, influence;
    bool coversReceiver = false;
    std::optional<FittedRegion> geometry;
    bool detailTolerant = false;
  };
  struct ReceiverSiteFit {
    static constexpr uint32_t maximumVariants = 6;
    uint32_t site = 0;
    std::vector<ReceiverProjection> variants;
  };
  struct ReceiverFits {
    bool available = false;
    uint32_t supported = 0, partitionRefused = 0, duplicates = 0;
    float detailToleranceMetres = 0;
    std::vector<ReceiverSiteFit> attempts;
  };
  // Reuse the normal geometry assessment with one shared ray set per local
  // origin. Frame-fenced readbacks must complete before finish is called.
  class ReceiverFitAudit {
  public:
    ReceiverFitAudit(ReceiverFits fits, std::span<const ReceiverSite> sites,
                     std::stop_token cancelled = {});
    std::span<const bake::Point> rays() const { return m_audit->rays(); }
    bool observe(uint32_t first, std::span<const bake::Result> results) { return m_audit->observe(first, results); }
    ReceiverFits finish() const;
    std::vector<FittedRegion> reassess(std::vector<FittedRegion> regions) const {
      return m_audit->reassess(std::move(regions));
    }
  private:
    ReceiverFits m_fits;
    std::unique_ptr<ProjectionAudit> m_audit;
  };
  // Bounded comparison with strict and optionally configured detail occupancy. Supported
  // hardware samples do not override partition walls. No capture is allocated,
  // and the resulting proxies still require an actual-geometry assessment.
  ReceiverFits fitReceiverSites(const scene::BakeScene &source,
      std::span<const ReceiverSample> receivers, const ReceiverSiteAudit &admission,
      const ReceiverSiteChecks &checks, uint32_t budget = 32, std::stop_token cancelled = {},
      float detailToleranceMetres = 0);
} // namespace remix::forward::probes
