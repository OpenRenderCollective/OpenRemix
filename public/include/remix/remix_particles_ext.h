// Optional particle emitters. No change to the retained API table or layouts.
#pragma once
#include "remix_c.h"

// Particle emitters: GPU-simulated billboards (rain, snow, sparks, dust)
// that the runtime spawns, moves and draws itself. Particles exist only
// through these calls (and the developer UI's own test emitters); the
// runtime never derives them from the game's content. They are drawn after
// the world is composed (after tonemapping and upscaling), depth-tested
// against the frame's opaque surfaces with a soft fade, under the view model
// and the UI. They are no part of the scene: never traced, never casting
// shadows or light, never in reflections. Particles are lit by the sun and
// the sky: with the path tracer, through a camera-centred grid of the sun's
// visibility (the NUMOS clouds' shadow and one shadow ray a cell, a quarter
// of the cells a frame) and the sky's mean radiance; with the raster backend, by the unshadowed sun and
// the scene's sky colour.
//
// Resolve the functions as named exports (GetProcAddress / dlsym). Every
// call takes the API lock and returns REMIXAPI_ERROR_CODE_NOT_INITIALIZED
// before Startup. Handles stay valid until destroyed or Shutdown.
//
// Units are the scene's (the camera's world units). Presets scale their
// sizes and speeds by rtx.native.pathtrace.sceneScale (metres per unit) and
// fall along the scene's up axis (rtx.native.pathtrace.volumetricUpAxis).
typedef uint64_t remixapi_ParticleEmitterHandleEXT; // 0: none

// Where particles spawn: at the transform's origin, in the box of half
// extents `extent` (emitter space), in the ball of radius extent.x (emitter
// space), or in a box of half extents `extent` around the camera. A camera
// volume ignores the transform: its emitter space is the world's axes turned
// so that +Z is the up axis, centred on a point a quarter of extent.z above
// the camera. Particles of a camera volume die when they leave it.
#define REMIXAPI_PARTICLE_SHAPE_POINT_EXT 0u
#define REMIXAPI_PARTICLE_SHAPE_BOX_EXT 1u
#define REMIXAPI_PARTICLE_SHAPE_SPHERE_EXT 2u
#define REMIXAPI_PARTICLE_SHAPE_CAMERA_VOLUME_EXT 3u

// A preset fills every field below except flags, maxParticles, transform
// and seed; spawnRate and extent keep their values when nonzero (then the
// preset's). RAIN: velocity-stretched streaks falling at about 9 m/s in a
// camera volume. SNOW: soft flakes drifting down at about 1 m/s.
#define REMIXAPI_PARTICLE_PRESET_NONE_EXT 0u
#define REMIXAPI_PARTICLE_PRESET_RAIN_EXT 1u
#define REMIXAPI_PARTICLE_PRESET_SNOW_EXT 2u

// FOLLOW_WEATHER: the spawn rate is scaled by the weather's precipitation,
// 0..1: while the NUMOS sky runs, the intensity in force there (the game's,
// remix_sky_ext.h, or its weather preset's); otherwise
// rtx.native.particles.precipitation (SetConfigVariable, the Sky tab).
// SHELTER (path tracer only): a particle is not spawned where the opaque
// world is above it, and dies where its initial direction of travel meets
// the opaque world (one ray each at spawn). Rain stays out of rooms and
// stops on roofs. The raster backend ignores it.
// CAMERA_SPACE: particles move with the camera (the camera's motion is
// added to theirs) instead of staying in the world.
// UNLIT: colour is shown as given (with emission), without the sun and sky.
#define REMIXAPI_PARTICLE_FOLLOW_WEATHER_EXT 0x1u
#define REMIXAPI_PARTICLE_SHELTER_EXT 0x2u
#define REMIXAPI_PARTICLE_CAMERA_SPACE_EXT 0x4u
#define REMIXAPI_PARTICLE_UNLIT_EXT 0x8u

// Blending over the frame, unsorted: ADDITIVE adds colour x alpha;
// PREMULTIPLIED composites colour x alpha over the frame by alpha.
#define REMIXAPI_PARTICLE_BLEND_ADDITIVE_EXT 0u
#define REMIXAPI_PARTICLE_BLEND_PREMULTIPLIED_EXT 1u

// CAMERA: square billboards facing the camera. VELOCITY: billboards facing
// the camera but stretched along the particle's motion as seen on screen,
// size wide and size + |velocity| x stretch long (streaks). HORIZONTAL:
// squares lying in the plane across the up axis (ripples, decals).
#define REMIXAPI_PARTICLE_FACING_CAMERA_EXT 0u
#define REMIXAPI_PARTICLE_FACING_VELOCITY_EXT 1u
#define REMIXAPI_PARTICLE_FACING_HORIZONTAL_EXT 2u

#define REMIXAPI_STRUCT_TYPE_PARTICLE_EMITTER_INFO_EXT ((remixapi_StructType)0x7fff1050)
typedef struct remixapi_ParticleEmitterInfoEXT {
  remixapi_StructType sType;
  const void* pNext;
  uint32_t shape;         // REMIXAPI_PARTICLE_SHAPE_*_EXT
  uint32_t preset;        // REMIXAPI_PARTICLE_PRESET_*_EXT
  uint32_t flags;         // REMIXAPI_PARTICLE_*_EXT flags
  uint32_t blend;         // REMIXAPI_PARTICLE_BLEND_*_EXT
  uint32_t facing;        // REMIXAPI_PARTICLE_FACING_*_EXT
  uint32_t maxParticles;  // live at once, up to 65536 (0: 32768 with a preset, else 4096); within the budget
  remixapi_Transform transform; // emitter space to world (rows)
  remixapi_Float3D extent;      // see the shapes
  float spawnRate;              // particles per second, >= 0
  float lifetimeMin, lifetimeMax; // seconds, 0 < min <= max
  // Initial velocity: velocity plus speed in a random direction within
  // coneAngleDegrees (0..180) of the emitter's +Z axis; velocity and the
  // axis are in emitter space (the transform's rotation applies).
  remixapi_Float3D velocity;
  float speed;
  float coneAngleDegrees;
  remixapi_Float3D acceleration; // world space, units/s^2 (gravity)
  float drag;                    // 1/s, >= 0: velocity decays by exp(-drag t)
  float turbulence;              // random acceleration, units/s^2, >= 0 (drift)
  float sizeStart, sizeEnd;      // billboard width over life, units, >= 0
  float sizeJitter;              // 0..1: each particle's size scaled by 1 - jitter..1
  float stretch;                 // VELOCITY facing: seconds of motion added to the length, >= 0
  remixapi_Float4D colorStart, colorEnd; // linear colour (lit like a diffuse surface) and alpha over life, >= 0
  remixapi_Float3D emission;     // radiance added, unlit, >= 0
  // A texture from CreateTexture sampled over the billboard (RGBA, its
  // alpha multiplies), or null for a soft round sprite.
  remixapi_TextureHandle texture;
  float softDistance;            // units over which particles fade into surfaces behind them, >= 0 (0: hard)
  uint32_t seed;
} remixapi_ParticleEmitterInfoEXT;

// Live counts are the last completed frame's.
typedef struct remixapi_ParticleStatisticsEXT {
  uint64_t structSize;       // caller sets to at least sizeof; written back
  uint32_t emitters;         // created (API and developer UI)
  uint32_t activeEmitters;   // enabled with a slice of the budget
  uint32_t liveParticles;
  uint32_t allocatedParticles; // slices given to emitters
  uint32_t budget, spawnBudget; // rtx.native.particles.budget / .spawnBudget
  uint32_t spawnsRequested;  // last frame: spawns asked for (rates and bursts, within the spawn budget)
  uint32_t spawnsDropped;    // last frame: spawns past an emitter's slice, the spawn budget or the budget
  uint32_t spawnsSheltered;  // last frame: spawns SHELTER kept out
  uint32_t simulatedFrames;
  float gpuMilliseconds;     // last frame's particle passes (needs rtx.native.gpuPassTimers), else 0
  uint32_t enabled;          // rtx.native.particles.enabled and a backend that draws them
} remixapi_ParticleStatisticsEXT;

#ifdef __cplusplus
extern "C" {
#endif
// Invalid arguments: a null pointer, a wrong sType, an unknown shape, preset,
// blend, facing or flag bit, maxParticles above 65536, values that are not
// finite or outside the ranges above. Creating beyond 256 emitters fails
// with GENERAL_FAILURE. An emitter starts enabled.
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_CreateParticleEmitterEXT)(
    const remixapi_ParticleEmitterInfoEXT* info, remixapi_ParticleEmitterHandleEXT* out);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateParticleEmitterEXT(
    const remixapi_ParticleEmitterInfoEXT* info, remixapi_ParticleEmitterHandleEXT* out);
// Replaces every parameter from the next frame; live particles keep moving
// and take the new appearance. A new maxParticles clears them.
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_UpdateParticleEmitterEXT)(
    remixapi_ParticleEmitterHandleEXT emitter, const remixapi_ParticleEmitterInfoEXT* info);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateParticleEmitterEXT(
    remixapi_ParticleEmitterHandleEXT emitter, const remixapi_ParticleEmitterInfoEXT* info);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_SetParticleEmitterTransformEXT)(
    remixapi_ParticleEmitterHandleEXT emitter, const remixapi_Transform* transform);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetParticleEmitterTransformEXT(
    remixapi_ParticleEmitterHandleEXT emitter, const remixapi_Transform* transform);
// A disabled emitter stops spawning; its live particles finish their lives.
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_SetParticleEmitterEnabledEXT)(
    remixapi_ParticleEmitterHandleEXT emitter, remixapi_Bool enabled);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetParticleEmitterEnabledEXT(
    remixapi_ParticleEmitterHandleEXT emitter, remixapi_Bool enabled);
// Spawns count particles at the next frame, on top of the rate, enabled or
// not (within the spawn budget).
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_BurstParticleEmitterEXT)(
    remixapi_ParticleEmitterHandleEXT emitter, uint32_t count);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_BurstParticleEmitterEXT(
    remixapi_ParticleEmitterHandleEXT emitter, uint32_t count);
// Removes the emitter's live particles at the next frame.
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_ClearParticleEmitterEXT)(
    remixapi_ParticleEmitterHandleEXT emitter);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ClearParticleEmitterEXT(
    remixapi_ParticleEmitterHandleEXT emitter);
// The emitter and its particles go at the next frame.
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_DestroyParticleEmitterEXT)(
    remixapi_ParticleEmitterHandleEXT emitter);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyParticleEmitterEXT(
    remixapi_ParticleEmitterHandleEXT emitter);
// Sets rtx.native.particles.budget (live particles of all emitters,
// 1024..262144) and rtx.native.particles.spawnBudget (spawns a frame,
// 64..65536). Emitters past the budget get no slice (no particles) until
// others go; a change of the budget clears every emitter.
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_SetParticleBudgetEXT)(
    uint32_t maxParticles, uint32_t maxSpawnsPerFrame);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetParticleBudgetEXT(
    uint32_t maxParticles, uint32_t maxSpawnsPerFrame);
typedef remixapi_ErrorCode (REMIXAPI_PTR *PFN_remixapi_GetParticleStatisticsEXT)(
    remixapi_ParticleStatisticsEXT* out);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetParticleStatisticsEXT(
    remixapi_ParticleStatisticsEXT* out);
#ifdef __cplusplus
}
#endif
