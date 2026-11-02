#pragma once
#include "forward/bake/transport.h"
#include <utility>

namespace remix::forward::bake {
  // Render-thread demand, offered only after the donor's worker/fence handoff.
  // It retains a scene only when a real requester accepts compatible inputs.
  class SceneRequest {
  public:
    enum class Use { Exact, Radiance };
    SceneRequest(std::shared_ptr<const scene::BakeScene> source, Options options, Use use = Use::Exact)
        : m_source(std::move(source)), m_options(std::move(options)), m_use(use) {}
    bool matches(const scene::BakeScene &source, const Options &options) const {
      return m_source.get() == &source && m_options == options;
    }
    bool offer(const Transport &donor) {
      if (m_lease || !m_source) return false;
      const bool compatible = m_use == Use::Radiance
          ? donor.canShareRadianceScene(*m_source, m_options) : donor.canShareScene(*m_source, m_options);
      if (!compatible) return false;
      m_lease = donor.leaseScene();
      return true;
    }
    Transport::SceneLease take() { return std::exchange(m_lease, {}); }
    Transport::Memory memory() const { return m_lease.memory(); }
  private:
    std::shared_ptr<const scene::BakeScene> m_source;
    Options m_options;
    Transport::SceneLease m_lease;
    Use m_use;
  };
}
