#include "forward/probes/receiver_visibility.h"
#include "forward/probes/receiver_pairs.h"
#include "forward/probes/receiver_audit.h"
#include "forward/probes/receiver_sites.h"
#include "forward/probes/receiver_site_checks.h"
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
  auto manyCaptures = captures;
  while (manyCaptures.size() < 8) {
    auto capture = captures[0];
    capture.position[0] = float(manyCaptures.size() * 10);
    capture.bounds.minimum[0] = capture.position[0] - 1;
    capture.bounds.maximum[0] = capture.position[0] + 1;
    manyCaptures.push_back(capture);
  }
  receivers[0].position = {0, 0, 0};
  const auto firstRound = p::receiverPairs(std::span(receivers).first(4), manyCaptures, .01f, 8, 4);
  require(firstRound.size() == 8);
  std::vector<V> states(8, V::Blocked);
  states[4] = V::Unknown;
  const auto retry = p::receiverRetryPairs(receivers, manyCaptures, firstRound, states, .01f, 8);
  require(retry.size() == 4);
  const auto limitedRetry = p::receiverRetryPairs(receivers, manyCaptures, firstRound, states, .01f, 1);
  require(limitedRetry.size() == 1 && limitedRetry[0].rank == 4);
  for (const auto &pair : retry) {
    require(pair.receiver == firstRound[0].receiver && pair.rank >= 4 && pair.rank < 8);
    for (const auto &old : firstRound)
      require(pair.receiver != old.receiver || pair.capture != old.capture);
  }
  states[0] = V::Visible;
  require(p::receiverRetryPairs(receivers, manyCaptures, firstRound, states, .01f, 8).empty());
  auto duplicate = firstRound;
  duplicate[1].capture = duplicate[0].capture;
  rejected = false;
  try { p::receiverRetryPairs(receivers, manyCaptures, duplicate, states, .01f, 8); }
  catch (const std::invalid_argument &) { rejected = true; }
  require(rejected);
  p::ReceiverAudit rounds(firstRound, .01f);
  rejected = false;
  try { rounds.append(retry); } catch (const std::logic_error &) { rejected = true; }
  require(rejected); // Pending GPU owners cannot be displaced by retry allocation.
  std::vector<Result> blocked(8, geometry(1));
  require(rounds.observe(0, blocked));
  rounds.append(retry);
  require(!rounds.complete() && rounds.visibility(0) == V::Blocked && rounds.rays().size() == 12);
  std::vector<Result> clear(4, geometry(10000));
  require(rounds.observe(8, clear) && rounds.complete() && rounds.visibility(8) == V::Visible);
  rejected = false;
  try { rounds.append({firstRound[0]}); } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected && rounds.complete() && rounds.rays().size() == 12);
  remix::scene::BakeScene source;
  source.sceneScale = .0254f;
  require(p::receiverSites(source, receivers, rounds).empty()); // A visible retry forbids unnecessary new sites.
  auto unsupported = std::vector<p::ReceiverPair>(firstRound.begin(), firstRound.begin() + 4);
  unsupported.insert(unsupported.end(), retry.begin(), retry.end());
  p::ReceiverAudit noCapture(unsupported, .01f);
  require(noCapture.observe(0, blocked));
  auto partition = std::make_shared<remix::scene::VisibilityPartition>();
  partition->closed = true; partition->clusterCount = 1; partition->leafClusters = {-1};
  partition->nodes.push_back({{0, 0, 1, 1000}, {-1, -1}});
  source.partition = partition;
  const auto sites = p::receiverSites(source, receivers, noCapture);
  require(sites.size() == 1 && sites[0].partitionDisagreement &&
              sites[0].receiver == firstRound[0].receiver &&
              std::abs(sites[0].point.position[2] - .75f / source.sceneScale) < 1e-4f);
  p::ReceiverSiteAudit admission({sites[0], sites[0], sites[0]});
  auto freeSite = geometry(0); freeSite.statistics = {0, .5f, 64, 0};
  auto insideSite = freeSite; insideSite.statistics = {.75f, .25f, 64, 0};
  auto invalidSite = freeSite; invalidSite.statistics[2] = 0;
  require(!admission.sampledFree(0) && admission.observe(2, {&invalidSite, 1}) &&
              admission.observe(0, {&freeSite, 1}) && admission.observe(1, {&insideSite, 1}));
  require(admission.complete() && admission.sampledFree(0) && admission.known(1) &&
              !admission.sampledFree(1) && !admission.known(2) && !admission.observe(0, {&freeSite, 1}));
  p::ReceiverSiteChecks connected(admission, receivers, source.sceneScale);
  require(connected.rays().size() == 7 && !connected.supported(0));
  std::vector<Result> clearChecks(7, geometry(1000));
  require(connected.observe(3, std::span(clearChecks).subspan(3)) && !connected.supported(0) &&
              connected.observe(0, std::span(clearChecks).first(3)) && connected.complete());
  require(connected.supported(0) && !connected.supported(1) && !connected.supported(2));
  p::ReceiverSiteChecks acrossWall(admission, receivers, source.sceneScale);
  clearChecks[0] = geometry(1);
  require(acrossWall.observe(0, clearChecks) && acrossWall.connection(0) == V::Blocked && !acrossWall.supported(0));
  p::ReceiverSiteChecks tooClose(admission, receivers, source.sceneScale);
  clearChecks[0] = geometry(1000); clearChecks[1] = geometry(.01f);
  require(tooClose.observe(0, clearChecks) && tooClose.clearance(0) == V::Blocked && !tooClose.supported(0));
  p::ReceiverSiteChecks capped(admission, receivers, source.sceneScale);
  clearChecks[0] = geometry(1, true); clearChecks[1] = geometry(1000);
  require(capped.observe(0, clearChecks) && capped.connection(0) == V::Unknown && !capped.supported(0));
  std::vector<p::ReceiverPair> several;
  for (uint32_t owner : {1u, 3u, 5u})
    for (auto pair : unsupported) { pair.receiver = owner; several.push_back(pair); }
  p::ReceiverAudit severalBlocked(several, .01f);
  std::vector<Result> allBlocked(several.size(), geometry(1));
  require(severalBlocked.observe(0, allBlocked));
  partition->nodes[0] = {{0, 1, 0, 4}, {-1, -2}};
  partition->leafClusters = {-1, 0};
  const auto priority = p::receiverSites(source, receivers, severalBlocked, 1);
  require(priority.size() == 1 && priority[0].receiver == 3 && !priority[0].partitionDisagreement);
  const auto diagnostics = p::receiverSites(source, receivers, severalBlocked, 3);
  require(diagnostics.size() == 3 && diagnostics[0].receiver == 1 && diagnostics[1].receiver == 3 &&
              diagnostics[2].receiver == 5 && diagnostics[2].partitionDisagreement);
  rejected = false;
  try { p::receiverSites(source, receivers, severalBlocked, 1, stop.get_token()); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected);
}
