#include "forward/probes/receiver_sites.h"
#include <algorithm>

namespace remix::forward::probes {
  std::vector<ReceiverSite> receiverSites(const scene::BakeScene &source,
      std::span<const ReceiverSample> receivers, const ReceiverAudit &audit,
      uint32_t budget, std::stop_token cancelled) {
    auto check = [&] {
      if (cancelled.stop_requested()) throw std::runtime_error("Receiver site planning superseded");
    };
    check();
    if (!audit.complete() || receivers.size() > 8192 || !budget || budget > 512 ||
        !std::isfinite(source.sceneScale) || source.sceneScale <= 0)
      throw std::invalid_argument("Invalid receiver site inputs or budget");
    struct State { uint32_t ranks = 0; bool blocked = true; };
    std::vector<State> states(receivers.size());
    for (size_t i = 0; i < audit.pairs().size(); ++i) {
      const auto &pair = audit.pairs()[i];
      if (pair.receiver >= receivers.size() || pair.rank >= 8)
        throw std::invalid_argument("Invalid receiver site ownership");
      auto &state = states[pair.receiver];
      if (state.ranks & (1u << pair.rank)) throw std::invalid_argument("Duplicate receiver site rank");
      state.ranks |= 1u << pair.rank;
      state.blocked &= audit.visibility(i) == ReceiverVisibility::Blocked;
    }
    std::array<std::vector<ReceiverSite>, 3> groups;
    for (uint32_t i = 0; i < states.size(); ++i) {
      check();
      if (states[i].ranks != 255 || !states[i].blocked) continue;
      const auto &receiver = receivers[i];
      const double length = std::hypot(receiver.normal[0], receiver.normal[1], receiver.normal[2]);
      if (!std::isfinite(length) || length == 0) continue;
      Vec3 position;
      for (uint32_t c = 0; c < 3; ++c)
        position[c] = float(receiver.position[c] + receiver.normal[c] / length * (.75 / source.sceneScale));
      const auto connection = receiverRay(receiver, position, .002f / source.sceneScale);
      if (!connection) continue;
      ReceiverSite site;
      site.receiver = i;
      std::copy(position.begin(), position.end(), site.point.position.begin());
      site.partitionDisagreement = source.partition && source.partition->closed &&
          source.partition->cluster(connection->point.position.data()) < 0;
      // A capture outside the partition cannot be fitted. Retain those sites
      // for diagnostics only after fit-capable proposals have their budget;
      // otherwise solid-side receivers can starve every useful candidate.
      const bool outside = source.partition && source.partition->closed &&
          source.partition->cluster(position.data()) < 0;
      groups[outside ? 2 : site.partitionDisagreement ? 0 : 1].push_back(site);
    }
    std::vector<ReceiverSite> result;
    for (const auto &group : groups) {
      const size_t count = std::min(group.size(), size_t(budget) - result.size());
      for (size_t i = 0; i < count; ++i) result.push_back(group[((2 * i + 1) * group.size()) / (2 * count)]);
    }
    return result;
  }
} // namespace remix::forward::probes
