#pragma once
#include "scene/types.h"
#include <memory>
#include <vector>

namespace remix::scene {
  // A remixapi_InstanceInfo whose pNext chain has been validated and flattened.
  // Mesh and material override stay handles; resolving them yields Draw records.
  struct InstanceDescription {
    uint64_t mesh = 0, materialOverride = 0;
    Matrix model = identity();
    // Hidden, ignored and first-person-shadow categories: validated, never drawn.
    bool hidden = false;
    uint32_t categoryFlags = 0;
    bool doubleSided = false;
    Blend blend;
    bool hasAlphaState = false, alphaTest = false;
    uint32_t alphaCompare = 7;
    float alphaReference = 0;
    bool hasRasterState = false, depthTest = true, depthWrite = true, disableColorWrites = false;
    uint32_t depthCompare = 3;
    // Draw defaults (no identity): frame-local, dynamic, raster visible.
    bool hasIdentity = false;
    uint64_t instanceId = 0;
    uint32_t classification = 1;
    bool rasterVisible = true;
    // A caller identity that takes part in the per-frame duplicate check.
    bool traceIdentity = false;
    bool instanced = false;
    // GPU instancing input. Borrowed from the caller: valid only during the call.
    const remixapi_Transform *instanceValues = nullptr;
    uint32_t instanceCount = 0;
    std::shared_ptr<std::vector<Matrix>> bones;
    // remixapi_InstanceInfoHaloEXT: a halo source, kept out of the scene.
    bool halo = false;
    bool wireframe = false; // remixapi_InstanceInfoWireframeEXT
    Halo haloParameters;
    // remixapi_InstanceInfoMaskEXT. maskSource: a coverage source, kept out of
    // the scene (maskParameters). Otherwise maskGroup != 0 marks a masked
    // raster-only overlay.
    bool maskSource = false;
    Mask maskParameters;
    uint8_t maskGroup = 0;
    bool maskOutside = false;
    // Role CUT: a scene instance like any other, with the group whose
    // coverage hides it from the camera.
    uint8_t cutGroup = 0;
    // remixapi_InstanceInfoCameraCullingEXT flags (camera rays only).
    uint8_t cameraCull = 0;
    // remixapi_InstanceInfoRasterOrderEXT (0: sorted by depth).
    uint32_t rasterOrder = 0;
    // remixapi_InstanceInfoSpriteEXT (0: none).
    uint32_t spriteFlags = 0;
    float spriteSoft[3] = {};
  };
  // Throws the codes DrawInstance returns. Mesh-independent checks only.
  void describeInstance(const remixapi_InstanceInfo &info, InstanceDescription &out);

  // A registered draw (remixapi_AddSceneInstancesEXT). The description is kept
  // chain-free; records are ready Draws, rebuilt only when the description, the
  // mesh or a material they were resolved from changes.
  struct RetainedInstance {
    uint64_t handle = 0;
    InstanceDescription description; // instanceValues is always null here
    std::vector<Matrix> locals;      // GPU instancing transforms, object space
    bool visible = true;
    bool callerIdentity = false; // InstanceIdentityEXT given: never reclassified
    bool placed = true;          // every world transform is finite
    bool watch = false;          // resolved without a live mesh or material handle
    bool moving = false;         // demoted to dynamic by a transform change
    uint64_t lastMove = 0;
    // Persistent static set (NativeResources::setPersistentStatic). revision
    // changes whenever the records do. member: drawn through the static set by
    // the last replay, from the records of memberRevision. logged: the handle
    // is in the consumer's change list.
    uint64_t revision = 0, memberRevision = 0;
    uint64_t resolvedModGeneration = 0;
    bool staticEligible = false, member = false, logged = false;
    bool staticEmissive = false; // a traced record has emission (Settings' local-light detection)
    uint64_t freshRetire = ~0ull; // NativeResources' retire generation it was last found not stale at
    uint32_t surfaceCount = 0;
    uint32_t replicaSlot = ~0u;   // publishFrame: its slot in the published replica, when still valid
    std::shared_ptr<Mesh> mesh;                       // what the mesh handle resolved to
    std::vector<std::shared_ptr<Material>> resolved;  // what the material handles resolved to
    std::vector<Draw> records;                        // instance-major, surfaceCount per instance
    uint32_t instanceCount() const { return description.instanced ? uint32_t(locals.size()) : 1u; }
  };

  // Statistics of the last DrawRetainedInstances call.
  struct RetainedStatistics {
    uint64_t registered = 0, appended = 0, draws = 0, resolved = 0, overCapacity = 0, moving = 0;
    // Persistent static set: members drawn by this replay and their draws
    // (both included above), and handles whose membership or records changed.
    uint64_t staticMembers = 0, staticDraws = 0, staticChanges = 0;
  };

  // Whether every record of a retained instance can be traced through the
  // persistent static set: static, traced, not animated, rigid and not a
  // streamed lineage. Such records have no per-frame UV animation, so their
  // trace inputs (emitters included) change only with the records.
  bool staticTraceEligible(const std::vector<Draw> &records);
} // namespace remix::scene
