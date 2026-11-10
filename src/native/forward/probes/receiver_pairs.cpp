#include "forward/probes/receiver_pairs.h"
#include <algorithm>

namespace remix::forward::probes {
  namespace {
    void check(std::stop_token cancelled) {
      if (cancelled.stop_requested()) throw std::runtime_error("Receiver pair planning superseded");
    }
    bool contains(const Capture &capture, Vec3 position) {
      const auto local = projectionPoint(capture.worldToLocal, position);
      const auto &bounds = capture.influence.valid ? capture.influence : capture.bounds;
      for (uint32_t c = 0; c < 3; ++c)
        if (local[c] < bounds.minimum[c] || local[c] > bounds.maximum[c]) return false;
      return true;
    }
  }
  std::vector<ReceiverPair> receiverPairs(std::span<const ReceiverSample> receivers,
      std::span<const Capture> captures, float bias, uint32_t rayBudget,
      uint32_t nearest, std::stop_token cancelled, uint32_t skipNearest) {
    check(cancelled);
    if (receivers.size() > 8192 || captures.size() > maxProbes || !rayBudget || rayBudget > 32768 ||
        !nearest || nearest > 4 || skipNearest > maxProbes || !std::isfinite(bias) || bias <= 0)
      throw std::invalid_argument("Invalid receiver pair budget or bias");
    for (const auto &capture : captures) validateCapture(capture);
    std::vector<uint32_t> uncovered;
    for (uint32_t i = 0; i < receivers.size(); ++i) {
      check(cancelled);
      for (uint32_t c = 0; c < 3; ++c)
        if (!std::isfinite(receivers[i].position[c]) || !std::isfinite(receivers[i].normal[c]))
          throw std::invalid_argument("Nonfinite receiver pair input");
      if (std::none_of(captures.begin(), captures.end(), [&](const auto &capture) {
            return contains(capture, receivers[i].position);
          })) uncovered.push_back(i);
    }
    if (skipNearest >= captures.size()) return {};
    const uint32_t perReceiver = std::min({nearest, rayBudget, uint32_t(captures.size()) - skipNearest});
    const size_t count = std::min(uncovered.size(), size_t(rayBudget / perReceiver));
    std::vector<ReceiverPair> result, candidates;
    result.reserve(count * perReceiver);
    candidates.reserve(captures.size());
    for (size_t i = 0; i < count; ++i) {
      check(cancelled);
      const auto receiver = uncovered[((2 * i + 1) * uncovered.size()) / (2 * count)];
      candidates.clear();
      for (uint32_t capture = 0; capture < captures.size(); ++capture)
        if (const auto ray = receiverRay(receivers[receiver], captures[capture].position, bias))
          candidates.push_back({receiver, capture, *ray});
      std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        return a.ray.distance < b.ray.distance;
      });
      for (size_t rank = skipNearest; rank < candidates.size() && rank < skipNearest + perReceiver; ++rank) {
        candidates[rank].rank = uint32_t(rank);
        result.push_back(candidates[rank]);
      }
    }
    return result;
  }
  std::vector<ReceiverPair> receiverRetryPairs(std::span<const ReceiverSample> receivers,
      std::span<const Capture> captures, std::span<const ReceiverPair> previous,
      std::span<const ReceiverVisibility> visibility, float bias, uint32_t rayBudget,
      std::stop_token cancelled) {
    check(cancelled);
    if (receivers.size() > 8192 || captures.size() > maxProbes || previous.size() > 32768 ||
        previous.size() != visibility.size())
      throw std::invalid_argument("Invalid previous receiver audit");
    struct State { uint64_t captures = 0; uint32_t ranks = 0; bool blocked = true; };
    std::vector<State> states(receivers.size());
    for (size_t i = 0; i < previous.size(); ++i) {
      check(cancelled);
      const auto &pair = previous[i];
      if (pair.receiver >= receivers.size() || pair.capture >= captures.size() || pair.rank >= 4)
        throw std::invalid_argument("Invalid receiver retry ownership");
      auto &state = states[pair.receiver];
      if (state.ranks & (1u << pair.rank)) throw std::invalid_argument("Duplicate receiver shortlist rank");
      if (state.captures & (uint64_t(1) << pair.capture))
        throw std::invalid_argument("Duplicate receiver shortlist capture");
      state.captures |= uint64_t(1) << pair.capture;
      state.ranks |= 1u << pair.rank;
      state.blocked &= visibility[i] == ReceiverVisibility::Blocked;
    }
    std::vector<ReceiverSample> retry;
    std::vector<uint32_t> owners;
    for (uint32_t i = 0; i < states.size(); ++i)
      if (states[i].ranks == 15 && states[i].blocked) {
        owners.push_back(i);
        retry.push_back(receivers[i]);
      }
    auto pairs = receiverPairs(retry, captures, bias, rayBudget, 4, cancelled, 4);
    for (auto &pair : pairs) pair.receiver = owners.at(pair.receiver);
    return pairs;
  }
} // namespace remix::forward::probes
