#include "forward/probes/composition.h"
#include "forward/probes/coverage_baseline.h"
#include "forward/probes/coverage_context.h"
#include "forward/probes/coverage_image_plan.h"
#include "forward/probes/identities.h"
#include <limits>
#include <stdexcept>

namespace p = remix::forward::probes;
namespace {
  void require(bool value) {
    if (!value) throw std::runtime_error("Reflection fallback composition invariant failed");
  }
  p::Record box(float radius) {
    p::Record record;
    record.influenceMinimum = {-radius, -radius, -radius, 0};
    record.influenceMaximum = {radius, radius, radius, 0};
    return record;
  }
}
void testProbeComposition() {
  {
    p::Capture automaticCapture;
    automaticCapture.bounds.include({-1,-1,-1}); automaticCapture.bounds.include({1,1,1});
    automaticCapture.resolution = 8;
    auto authoredCapture = automaticCapture;
    authoredCapture.position = {10,20,0};
    authoredCapture.worldToLocal = {{{0,-1,0,20},{1,0,0,-10},{0,0,1,0}}};
    authoredCapture.priority = -32768;
    p::Header authoredHeader, automaticHeader;
    authoredHeader.parameters = automaticHeader.parameters = {1,8,4,510};
    authoredHeader.probes[0] = p::projectionRecord(authoredCapture);
    automaticHeader.probes[0] = p::projectionRecord(automaticCapture);
    auto composition = p::compose(authoredHeader, automaticHeader);
    const auto snapshot = p::composedCoverageBaseline(composition, 100, 11);
    require(snapshot.composed && snapshot.volumes.size() == 2 && snapshot.volumes[0].protectedCapture &&
            !snapshot.volumes[1].protectedCapture && !snapshot.volumes[0].source.automatic &&
            snapshot.volumes[1].source.automatic && snapshot.volumes[1].source.layer == 0);
    require(snapshot.volumes[0].position() == authoredCapture.position &&
            snapshot.volumes[0].record.worldToLocal == authoredCapture.worldToLocal &&
            composition.header.probes[1].minimum[3] == -32769);
    remix::scene::BakeScene scene;
    scene.sceneId = 100; scene.revision = 7; scene.contentHash = 1234;
    scene.chartRevision = 2; scene.reflectionRevision = 3; scene.ready = true;
    const auto context = p::coveragePlanContext(scene, snapshot);
    require(p::matchesCoverageContext(context, scene, snapshot));
    require(!p::matchesCoverageContext({}, scene, snapshot));
    auto wrongMap = scene; ++wrongMap.sceneId;
    bool wrongMapRejected = false;
    try { (void)p::coveragePlanContext(wrongMap, snapshot); }
    catch (const std::invalid_argument &) { wrongMapRejected = true; }
    require(wrongMapRejected);
    auto changedScene = scene; ++changedScene.contentHash;
    require(!p::matchesCoverageContext(context, changedScene, snapshot));
    changedScene = scene; ++changedScene.revision;
    require(!p::matchesCoverageContext(context, changedScene, snapshot));
    changedScene = scene; ++changedScene.chartRevision;
    require(!p::matchesCoverageContext(context, changedScene, snapshot));
    changedScene = scene; changedScene.sceneScale = 2;
    require(!p::matchesCoverageContext(context, changedScene, snapshot));
    auto changedBaseline = snapshot; ++changedBaseline.authoredRevision;
    require(!p::matchesCoverageContext(context, scene, changedBaseline));
    changedBaseline = snapshot; changedBaseline.volumes[0].record.position[2] = -0.f;
    require(!p::matchesCoverageContext(context, scene, changedBaseline));
    changedBaseline = snapshot; ++changedBaseline.volumes[1].source.layer;
    require(!p::matchesCoverageContext(context, scene, changedBaseline));
    changedBaseline = snapshot; changedBaseline.volumes[0].record.influenceMaximum[0] += 1;
    require(!p::matchesCoverageContext(context, scene, changedBaseline));
    changedBaseline = snapshot; changedBaseline.volumes[0].record.minimum[0] -= .01f;
    require(!p::matchesCoverageContext(context, scene, changedBaseline));
    changedBaseline = snapshot; changedBaseline.volumes[0].record.maximum[3] += 1;
    require(!p::matchesCoverageContext(context, scene, changedBaseline));
    changedBaseline = snapshot; ++changedBaseline.parameters[1];
    require(!p::matchesCoverageContext(context, scene, changedBaseline));
    // Prepared captures may replace automatic layers, never authored layers.
    auto localCapture = automaticCapture;
    localCapture.position = {3,0,0};
    localCapture.bounds.minimum[0] = 2; localCapture.bounds.maximum[0] = 4;
    p::CoverageReadyImage local;
    local.header.parameters = automaticHeader.parameters;
    local.header.probes[0] = p::projectionRecord(localCapture);
    local.context = context; local.layers = {{0,0}};
    p::ReceiverCoveragePlan selection;
    selection.context = context; selection.owners = {{0,0}}; selection.errors = {.1f};
    selection.candidateRecords = {local.header.probes[0]}; selection.replacements = {{0,1,1}};
    p::LayerIdentities ids{}; ids[0] = 123;
    auto prepare = [&] { return p::buildCoverageImagePlan(scene, snapshot, selection, composition.header, ids, local); };
    const auto copied = prepare();
    require(copied.header.parameters[0] == 2 && copied.identities[0] == 123 && copied.identities[1] == 0 &&
            copied.header.probes[0].position == composition.header.probes[0].position &&
            copied.header.probes[1].position[0] == 3 && copied.header.probes[1].minimum[3] == -32769 &&
            copied.copies[0].source == p::CoverageImageSource::Baseline &&
            copied.copies[1].source == p::CoverageImageSource::Local);
    selection.replacements[0].baseline = UINT32_MAX;
    require(prepare().header.parameters[0] == 3);
    auto rejectsImage = [&](auto action) {
      bool failed = false;
      try { action(); } catch (const std::invalid_argument &) { failed = true; }
      require(failed);
    };
    selection.replacements[0].baseline = 0;
    rejectsImage(prepare);
    selection.replacements[0].baseline = 1;
    local.header.probes[0].position[0] = 4;
    rejectsImage(prepare);
    local.header.probes[0].position[0] = 3;
    ++local.context.revision;
    rejectsImage(prepare);
    local.context = context;
    local.layers.push_back({0,0});
    rejectsImage(prepare);
    local.layers.pop_back();
    selection.maximumError = std::numeric_limits<float>::quiet_NaN();
    rejectsImage(prepare);
    selection.maximumError = .2f;
    auto authoredOnly = snapshot; authoredOnly.automaticAllowed = false;
    selection.context = local.context = p::coveragePlanContext(scene, authoredOnly);
    rejectsImage([&] { p::buildCoverageImagePlan(scene, authoredOnly, selection, composition.header, ids, local); });
    const auto publishedAutomatic = p::automaticCoverageBaseline(automaticHeader);
    require(!publishedAutomatic.composed && !publishedAutomatic.disabled &&
            publishedAutomatic.volumes[0].record.position == automaticHeader.probes[0].position);
    composition.header.probes[0].position[0] = 99;
    require(snapshot.volumes[0].record.position[0] == 10);
    auto duplicate = p::compose(authoredHeader, automaticHeader);
    duplicate.layers[1] = duplicate.layers[0];
    bool rejected = false;
    try { (void)p::composedCoverageBaseline(duplicate, 100, 11); } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected);
    duplicate = p::compose(authoredHeader, automaticHeader);
    duplicate.layers.pop_back();
    rejected = false;
    try { (void)p::composedCoverageBaseline(duplicate, 100, 11); } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected);
    auto disabledHeader = authoredHeader;
    disabledHeader.parameters[0] = 0;
    const auto disabled = p::composedCoverageBaseline(p::compose(disabledHeader, automaticHeader), 100, 11);
    require(disabled.composed && disabled.disabled && disabled.volumes.empty());
    const auto plain = p::automaticCoverageBaseline(std::span(&automaticCapture, 1));
    require(!plain.composed && plain.volumes.size() == 1 && !plain.volumes[0].protectedCapture);
  }

  {
    p::Header canonical;
    canonical.parameters[0] = 3;
    canonical.probes[0].minimum[3] = -2;
    canonical.probes[1].minimum[3] = 7;
    canonical.probes[2].minimum[3] = 7;
    const std::array<uint64_t, 3> ids{UINT64_MAX, 72, 171};
    const auto sorted = p::imageIdentities(canonical, ids);
    require(sorted[0] == 72 && sorted[1] == 171 && sorted[2] == UINT64_MAX && sorted[3] == 0);
    p::Composition plan;
    plan.header.parameters[0] = 4;
    plan.layers = {{false, 2}, {true, 0}, {false, 1}, {false, 0}};
    const auto composed = p::composedIdentities(plan, std::span(sorted).first(3));
    require(composed[0] == UINT64_MAX && composed[1] == 0 && composed[2] == 171 && composed[3] == 72);
    auto rejects = [&](auto action) {
      bool rejected = false;
      try { action(); } catch (const std::invalid_argument &) { rejected = true; }
      require(rejected);
    };
    rejects([&] { p::imageIdentities(canonical, std::span(ids).first(2)); });
    rejects([&] { p::imageIdentities(canonical, std::array<uint64_t, 3>{1, 1, 2}); });
    rejects([&] { p::imageIdentities(canonical, std::array<uint64_t, 3>{0, 1, 2}); });
    plan.layers[0].layer = 3;
    rejects([&] { p::composedIdentities(plan, std::span(sorted).first(3)); });
    plan.layers[0].layer = 0;
    rejects([&] { p::composedIdentities(plan, sorted); });
    plan.layers.pop_back();
    rejects([&] { p::composedIdentities(plan, sorted); });
  }
  p::Header authored, automatic;
  authored.parameters = {1, 128, 8, 131070};
  automatic.parameters = {3, 128, 8, 131070};
  authored.probes[0] = box(4);
  automatic.probes[0] = box(2);
  automatic.probes[1] = box(5);
  automatic.probes[2] = box(6);
  auto result = p::compose(authored, automatic);
  require(result.covered == 1 && result.automatic == 2 && result.omitted == 0);
  require(result.header.parameters[0] == 3 && !result.layers[0].automatic);
  require(result.layers[1].automatic && result.layers[1].layer == 1 && result.layers[2].layer == 2);
  require(result.header.probes[1].minimum[3] == -1);
  result = p::compose(authored, automatic, 2);
  require(result.covered == 1 && result.automatic == 1 && result.omitted == 1);
  authored.probes[0].minimum[3] = -32768;
  require(p::compose(authored, automatic).header.probes[1].minimum[3] == -32769);
  // Different translated/rotated frames still prove full containment.
  automatic.parameters[0] = 1;
  automatic.probes[0].worldToLocal = {{{0, 1, 0, 1}, {-1, 0, 0, 0}, {0, 0, 1, 0}}};
  require(p::compose(authored, automatic).covered == 1);
  automatic.probes[0].worldToLocal[0][3] = 4;
  require(p::compose(authored, automatic).automatic == 1);
  // Unknown or malformed bounds must never erase fallback coverage.
  automatic.probes[0] = box(2);
  automatic.probes[0].influenceMinimum[0] = std::numeric_limits<float>::quiet_NaN();
  require(p::compose(authored, automatic).automatic == 1);
  authored.parameters[0] = 0;
  require(p::compose(authored, automatic).layers.empty());
  authored.parameters[0] = 64;
  for (auto &record : authored.probes) record = box(1);
  result = p::compose(authored, automatic);
  require(result.layers.size() == 64 && result.omitted == 1 && !result.automatic);
  automatic.parameters[1] = 64;
  bool rejected = false;
  try { p::compose(authored, automatic); } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected);
}
