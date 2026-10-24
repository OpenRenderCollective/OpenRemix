#pragma once
#include "scene/types.h"
#include <remix/remix_native_ext.h>

namespace remix::trace {
  struct InstanceInput {
    uint64_t id = 0, geometry = 0;
    uint32_t sub = 0, customIndex = 0;
    scene::Matrix model = scene::identity();
    float diagonal = 0;
    uint64_t boundsRevision = 0;
    // No surface of the instance is double-sided (doubleSided belongs to the
    // whole instance). Selects hardware facing culling; see instanceFlags.
    bool oneSided = false;
    // Every surface is a blended, non-decal layer (glass, sprites): the
    // instance carries blendedInstanceMask, which shadow and bounce rays may
    // exclude (rtx.native.pathtrace.skipBlendedSecondaryRays).
    bool blended = false;
    // A view model (Draw::viewModel): viewModelInstanceMask only.
    bool viewModel = false;
    // Draw::reflectionOnly: reflectionOnlyInstanceMask only.
    bool reflectionOnly = false;
    // Every surface is a decal (paint): decalInstanceMask only, which
    // shadow and light-visibility rays exclude (they pass paint anyway).
    bool decal = false;
    bool operator==(const InstanceInput &) const = default;
  };
  // VkGeometryInstanceFlagBitsKHR values (static_assert-ed in tlas.cpp):
  // TRIANGLE_FACING_CULL_DISABLE and TRIANGLE_FRONT_COUNTERCLOCKWISE.
  constexpr uint32_t instanceFacingCullDisable = 1, instanceFrontCounterClockwise = 2;
  // An instance's flags: the model's handedness selects the front face (the
  // face rayQueryGetIntersectionFrontFaceEXT reports). Without hardware face
  // culling every instance disables facing culling and the shaders reject
  // one-sided back faces; with it (rtx.native.pathtrace.hardwareFaceCulling)
  // one-sided instances leave that to gl_RayFlagsCullBackFacingTrianglesEXT,
  // which culls exactly the triangles whose front-face query would be false.
  inline uint32_t instanceFlags(const InstanceInput &input, bool hardwareFaceCulling) {
    const auto &m = input.model;
    const float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                      m[8] * (m[1] * m[6] - m[5] * m[2]);
    return (hardwareFaceCulling && input.oneSided ? 0u : instanceFacingCullDisable) |
           (det > 0 ? instanceFrontCounterClockwise : 0u);
  }
  struct TlasPolicy {
    std::vector<InstanceInput> previous, built;
    uint32_t refits = 0;
    bool initialized = false;
    // NONE + unchanged means no GPU work; NONE + changed means an UPDATE.
    uint32_t reason(const std::vector<InstanceInput> &inputs, bool dynamic, bool force) const;
    bool changed(const std::vector<InstanceInput> &inputs) const;
    void commit(const std::vector<InstanceInput> &inputs, uint32_t reason);
  };
  // Instance masks: the separate static and dynamic TLASes keep every bit but
  // the view models'; the combined one tags its parts so a query could select
  // either (every query includes both, as the two traversals did).
  // View models: only camera rays (and rays of paths that start on one)
  // include this bit; no other instance carries it.
  constexpr uint8_t viewModelInstanceMask = 8;
  // Reflection-only instances (Draw::reflectionOnly): only bounce rays include
  // this bit; no other instance carries it.
  constexpr uint8_t reflectionOnlyInstanceMask = 16;
  constexpr uint8_t allInstancesMask = 0xff & ~viewModelInstanceMask & ~reflectionOnlyInstanceMask,
                    staticInstanceMask = 1, dynamicInstanceMask = 2,
                    blendedInstanceMask = 4, decalInstanceMask = 32;
  inline uint8_t instanceMask(bool combined, bool dynamic) {
    return !combined ? allInstancesMask : dynamic ? dynamicInstanceMask : staticInstanceMask;
  }
  // rtx.native.pathtrace.combinedTlas: one TLAS over the static instances
  // followed by the dynamic ones. Any static change rebuilds it (as it
  // rebuilds the static TLAS); the dynamic part follows the dynamic TLAS
  // policy: refit when only transforms moved, rebuild for membership,
  // geometry, the refit limit, displacement or a forced rebuild.
  struct CombinedTlasPolicy {
    TlasPolicy staticPart, dynamicPart;
    uint64_t staticGeneration = 0; // the static prefix generation staticPart holds; 0: compare
    enum Action { None, Update, Build };
    struct Decision {
      Action action = None;
      uint32_t reason = REMIXAPI_REBUILD_NONE_EXT;
      bool staticChanged = false;
    };
    // generation: nonzero when staticInputs are the frame tables' static prefix
    // of that generation; a generation already committed is not compared again.
    Decision decide(const std::vector<InstanceInput> &staticInputs, uint64_t generation,
                    const std::vector<InstanceInput> &dynamicInputs, bool force) const;
    // After the decided work (none, refit or build) was recorded.
    void commit(const std::vector<InstanceInput> &staticInputs, uint64_t generation,
                const std::vector<InstanceInput> &dynamicInputs, const Decision &decision);
  };
} // namespace remix::trace
