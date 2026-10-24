#include "pathtrace/tlas_policy.h"
#include <cstdio>
#include <cstdlib>

namespace {
  void require(bool value, const char *message) { if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); } }
  void cpu() {
    using namespace remix::trace;
    TlasPolicy policy;
    std::vector<InstanceInput> inputs = {{1, 10, 0, 0, remix::scene::identity(), 2}};
    auto reason = policy.reason(inputs, true, false);
    require(reason == REMIXAPI_REBUILD_MEMBERSHIP_EXT, "Initial TLAS build"); policy.commit(inputs, reason);
    require(!policy.changed(inputs) && !policy.reason(inputs, true, false), "Warm scene did work");
    for (unsigned i = 0; i < 300; ++i) {
      inputs[0].model[12] += .001f;
      require(policy.changed(inputs) && !policy.reason(inputs, true, false), "Rigid motion must refit"); policy.commit(inputs, 0);
    }
    inputs[0].model[12] += .001f;
    require(policy.reason(inputs, true, false) == REMIXAPI_REBUILD_REFIT_LIMIT_EXT, "Recovery refit limit");
    policy.commit(inputs, REMIXAPI_REBUILD_REFIT_LIMIT_EXT);
    inputs[0].model[12] += 17;
    require(policy.reason(inputs, true, false) == REMIXAPI_REBUILD_DISPLACEMENT_EXT, "Displacement recovery");
    inputs[0].geometry = 11;
    require(policy.reason(inputs, true, false) == REMIXAPI_REBUILD_GEOMETRY_EXT, "Geometry replacement rebuild");
    inputs[0].id = 2;
    require(policy.reason(inputs, true, false) == REMIXAPI_REBUILD_MEMBERSHIP_EXT, "Object lifetime rebuild");
    require(policy.reason(inputs, true, true) == REMIXAPI_REBUILD_FORCED_EXT, "Forced rebuild");
    inputs.clear(); require(policy.reason(inputs, true, false) == REMIXAPI_REBUILD_MEMBERSHIP_EXT, "Actual count, not capacity");
    // Instance flags: handedness picks the front face; hardware face culling
    // leaves facing culling on only for one-sided instances.
    InstanceInput facing{1, 10, 0, 0, remix::scene::identity(), 2};
    require(instanceFlags(facing, false) == (instanceFacingCullDisable | instanceFrontCounterClockwise) &&
                instanceFlags(facing, true) == (instanceFacingCullDisable | instanceFrontCounterClockwise),
            "Double-sided instance flags changed");
    facing.oneSided = true;
    require(instanceFlags(facing, false) == (instanceFacingCullDisable | instanceFrontCounterClockwise),
            "One-sided instance culls without hardware face culling");
    require(instanceFlags(facing, true) == instanceFrontCounterClockwise,
            "One-sided instance does not cull with hardware face culling");
    facing.model[0] = -1; // Mirrored: clockwise front faces.
    require(instanceFlags(facing, true) == 0 && instanceFlags(facing, false) == instanceFacingCullDisable,
            "Mirrored instance kept counterclockwise front faces");
    facing.model[0] = 0; // Degenerate: as before, not counterclockwise.
    require(instanceFlags(facing, true) == 0, "Degenerate instance changed facing");
    std::vector<InstanceInput> sided = {{1, 10, 0, 0, remix::scene::identity(), 2}};
    TlasPolicy facingPolicy;
    facingPolicy.commit(sided, facingPolicy.reason(sided, true, false));
    sided[0].oneSided = true;
    require(facingPolicy.reason(sided, true, false) == REMIXAPI_REBUILD_GEOMETRY_EXT &&
                facingPolicy.reason(sided, false, false) == REMIXAPI_REBUILD_GEOMETRY_EXT,
            "A facing change was refitted");
    // Combined TLAS: masks tag the parts; static changes rebuild, dynamic
    // motion refits, dynamic membership rebuilds, a known generation skips
    // the static comparison.
    require(instanceMask(false, false) == allInstancesMask && instanceMask(false, true) == allInstancesMask &&
                !(allInstancesMask & viewModelInstanceMask) && (allInstancesMask & skyOccluderInstanceMask) &&
                instanceMask(true, false) == (staticInstanceMask | skyOccluderInstanceMask) &&
                instanceMask(true, true) == (dynamicInstanceMask | skyOccluderInstanceMask) &&
                (staticInstanceMask & dynamicInstanceMask) == 0 &&
                !((viewModelInstanceMask | reflectionOnlyInstanceMask | blendedInstanceMask | decalInstanceMask) &
                  skyOccluderInstanceMask),
            "Instance masks");
    using Combined = CombinedTlasPolicy;
    Combined combined;
    std::vector<InstanceInput> statics = {{1, 10, 0, 0, remix::scene::identity(), 2},
                                          {2, 11, 0, 1, remix::scene::identity(), 2}};
    std::vector<InstanceInput> dynamics = {{3, 12, 0, 2, remix::scene::identity(), 2}};
    auto decision = combined.decide(statics, 7, dynamics, false);
    require(decision.action == Combined::Build && decision.staticChanged &&
                decision.reason == REMIXAPI_REBUILD_MEMBERSHIP_EXT,
            "Initial combined build");
    combined.commit(statics, 7, dynamics, decision);
    decision = combined.decide(statics, 7, dynamics, false);
    require(decision.action == Combined::None && !decision.staticChanged, "Warm combined scene did work");
    combined.commit(statics, 7, dynamics, decision);
    dynamics[0].model[12] += .5f;
    decision = combined.decide(statics, 7, dynamics, false);
    require(decision.action == Combined::Update && !decision.reason, "Dynamic motion must refit");
    combined.commit(statics, 7, dynamics, decision);
    require(combined.dynamicPart.refits == 1, "Combined refit count");
    dynamics[0].model[12] += 100;
    decision = combined.decide(statics, 7, dynamics, false);
    require(decision.action == Combined::Build && decision.reason == REMIXAPI_REBUILD_DISPLACEMENT_EXT &&
                !decision.staticChanged,
            "Dynamic displacement must rebuild");
    combined.commit(statics, 7, dynamics, decision);
    require(combined.dynamicPart.refits == 0, "A rebuild restarts refits");
    // A known static generation is not compared again (the frame tables
    // guarantee equal bytes); an unknown one is.
    auto moved = statics;
    moved[1].model[12] += 1;
    decision = combined.decide(moved, 7, dynamics, false);
    require(decision.action == Combined::None, "Known static generation was compared");
    decision = combined.decide(moved, 8, dynamics, false);
    require(decision.action == Combined::Build && decision.staticChanged &&
                decision.reason == REMIXAPI_REBUILD_STATIC_TRANSFORM_EXT,
            "Static transform must rebuild the combined TLAS");
    decision = combined.decide(statics, 0, dynamics, false);
    require(decision.action == Combined::None && !decision.staticChanged, "Equal statics without a generation");
    combined.commit(statics, 0, dynamics, decision);
    require(combined.staticGeneration == 0, "Generation 0 is never known");
    decision = combined.decide(moved, 0, dynamics, true);
    require(decision.action == Combined::Build && decision.staticChanged &&
                decision.reason == REMIXAPI_REBUILD_FORCED_EXT,
            "Forced combined rebuild");
    combined.commit(moved, 0, dynamics, decision);
    dynamics.push_back({4, 13, 0, 3, remix::scene::identity(), 2});
    decision = combined.decide(moved, 0, dynamics, false);
    require(decision.action == Combined::Build && decision.reason == REMIXAPI_REBUILD_MEMBERSHIP_EXT &&
                !decision.staticChanged,
            "Dynamic membership must rebuild");
    combined.commit(moved, 0, dynamics, decision);
    dynamics[1].oneSided = true;
    decision = combined.decide(moved, 0, dynamics, false);
    require(decision.action == Combined::Build && decision.reason == REMIXAPI_REBUILD_GEOMETRY_EXT,
            "A dynamic facing change was refitted");
    auto matrix = remix::scene::identity(); matrix[12] = 1024; matrix[13] = -64;
    require(remix::scene::multiply(matrix, remix::scene::inverse(matrix)) == remix::scene::identity(), "Separate camera/object inverse");
  }
}
int main() { cpu(); }
