#pragma once
#include "forward/probes/reference_provenance.h"

namespace remix::forward::probes {
  // Source 2 static combined-light reference convention. Sampling counts,
  // projection bounds and output fingerprints do not change the incident-light
  // components represented by this domain. Other component recipes need their
  // own domain, even when their units are also irradiance / pi.
  inline ReferenceProvenance source2Reference(uint64_t frozenLightingInputs) {
    return frozenLightingInputs ? ReferenceProvenance{1, frozenLightingInputs} : ReferenceProvenance{};
  }
}
