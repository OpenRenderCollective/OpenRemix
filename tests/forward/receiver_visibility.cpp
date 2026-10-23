#include "forward/probes/receiver_visibility.h"
#include "forward/probes/receiver_pairs.h"
#include "forward/probes/receiver_audit.h"
#include <limits>
#include <stdexcept>

namespace p = remix::forward::probes;
using remix::forward::bake::Result;
void require(bool value) { if (!value) throw std::runtime_error("Receiver visibility invariant failed"); }
Result geometry(float distance, bool missed = false, bool back = false) {
  Result value{};
  value.irradiance[0][3] = distance;
  value.statistics = {float(back), float(missed), 1, 0};
  return value;
}
int main() {
  p::ReceiverSample receiver;
  receiver.position = {10, 20, -144}; receiver.normal = {0, 0, 2};
  const auto ray = p::receiverRay(receiver, {10, 20, -140}, .05f);
  require(ray && std::abs(ray->point.position[2] + 143.95f) < 1e-5f &&
              std::abs(ray->distance - 3.95f) < 1e-5f && ray->point.normal[2] == 1);
  require(!p::receiverRay(receiver, {10, 20, -145}, .05f));
  require(!p::receiverRay(receiver, {10, 20, -143.95f}, .05f));
  using V = p::ReceiverVisibility;
  require(p::receiverVisibility(*ray, geometry(2), .01f) == V::Blocked);
  require(p::receiverVisibility(*ray, geometry(2, false, true), .01f) == V::Blocked);
  require(p::receiverVisibility(*ray, geometry(5), .01f) == V::Visible);
  require(p::receiverVisibility(*ray, geometry(5, true), .01f) == V::Visible);
  require(p::receiverVisibility(*ray, geometry(2, true), .01f) == V::Unknown);
  require(p::receiverVisibility(*ray, geometry(ray->distance), .01f) == V::Unknown);
  auto invalid = geometry(5);
  invalid.statistics[2] = 0;
  require(p::receiverVisibility(*ray, invalid, .01f) == V::Unknown);
  require(p::receiverVisibility(*ray, geometry(5, true, true), .01f) == V::Unknown);
  require(p::receiverVisibility(*ray, geometry(std::numeric_limits<float>::infinity()), .01f) == V::Unknown);
  auto scaled = receiver;
  for (auto &coordinate : scaled.position) coordinate *= 40;
  const auto scaledRay = p::receiverRay(scaled, {400, 800, -5600}, 2);
  require(scaledRay && std::abs(scaledRay->distance - ray->distance * 40) < .001f &&
              p::receiverVisibility(*scaledRay, geometry(80), .4f) == V::Blocked &&
              p::receiverVisibility(*scaledRay, geometry(200, true), .4f) == V::Visible);
  receiver.normal = {};
  require(!p::receiverRay(receiver, {10, 20, -140}, .05f));
  bool rejected = false;
  try { p::receiverRay(receiver, {0, 0, 0}, 0); } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected);
  std::vector<p::Capture> captures(3);
  for (uint32_t i = 0; i < captures.size(); ++i) {
    auto &capture = captures[i];
    capture.position = {float(i == 2 ? 5 : 0), 0, 10};
    capture.resolution = 8;
    capture.bounds.include({capture.position[0] - 1, -1, 9});
    capture.bounds.include({capture.position[0] + 1, 1, 11});
  }
  std::vector<p::ReceiverSample> receivers(200);
  for (uint32_t i = 0; i < receivers.size(); ++i) {
    receivers[i].position = {0, float(i), 0};
    receivers[i].normal = {0, 0, 1};
  }
  const auto pairs = p::receiverPairs(receivers, captures, .01f, 8, 2);
  require(pairs.size() == 8 && pairs[0].receiver == 25 && pairs[6].receiver == 175);
  for (uint32_t i = 0; i < pairs.size(); ++i) require(pairs[i].capture == i % 2);
  const auto repeated = p::receiverPairs(receivers, captures, .01f, 8, 2);
  for (uint32_t i = 0; i < pairs.size(); ++i)
    require(pairs[i].receiver == repeated[i].receiver && pairs[i].capture == repeated[i].capture);
  receivers[0].position = {0, 0, 10};
  const auto covered = p::receiverPairs(std::span(receivers).first(1), captures, .01f);
  require(covered.empty());
  auto rotated = captures[0];
  rotated.position = {10, 20, 10};
  rotated.worldToLocal = {{{0, 1, 0, -20}, {-1, 0, 0, 10}, {0, 0, 1, 0}}};
  rotated.influence.include({-2, -2, 8});
  rotated.influence.include({2, 2, 12});
  auto edgeReceiver = receivers[0];
  edgeReceiver.position = {10, 21.5f, 10};
  require(p::receiverPairs(std::span(&edgeReceiver, 1), std::span(&rotated, 1), .01f).empty());
  // Influence membership, not the smaller projection proxy, decides whether a query is needed.
  const auto single = p::receiverPairs(receivers, std::span(captures).first(1), .01f, 8, 4);
  require(single.size() == 8); // A single available capture must not waste a four-pair allocation.
  std::stop_source stop;
  stop.request_stop();
  rejected = false;
  try { p::receiverPairs(receivers, captures, .01f, 8, 2, stop.get_token()); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected);
  rejected = false;
  try { p::receiverPairs(receivers, captures, .01f, 0); }
  catch (const std::invalid_argument &) { rejected = true; }
  require(rejected);
  p::ReceiverAudit audit(pairs, .01f);
  std::vector<Result> hits(pairs.size());
  for (size_t i = 0; i < hits.size(); ++i) hits[i] = geometry(pairs[i].ray.distance + 1);
  require(!audit.complete() && audit.observe(4, std::span(hits).subspan(4)));
  require(!audit.observe(3, std::span(hits).subspan(3, 2))); // Refuse overlap atomically, including unseen index 3.
  require(audit.observe(0, std::span(hits).first(4)) && audit.complete());
  require(audit.visibility(3) == V::Visible && !audit.observe(0, std::span(hits).first(1)));
}
