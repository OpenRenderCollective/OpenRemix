// Optional native-client extensions. The Remix 0.1003 interface table is unchanged.
#pragma once
#include "remix_c.h"

// Optional LightInfo pNext. A backend may use this hint when choosing static
// lighting representation. Absence preserves its normal policy. Moving and
// styled lights remain live regardless of a baked preference.
#define REMIXAPI_STRUCT_TYPE_LIGHT_BAKING_EXT ((remixapi_StructType)0x7fff1075)
typedef enum remixapi_LightBakingHintEXT {
  REMIXAPI_LIGHT_BAKING_DEFAULT_EXT = 0,
  REMIXAPI_LIGHT_BAKING_PREFER_BAKED_EXT = 1,
  REMIXAPI_LIGHT_BAKING_REQUIRE_MIXED_EXT = 2
} remixapi_LightBakingHintEXT;

typedef struct remixapi_LightInfoBakingEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_LightBakingHintEXT hint;
} remixapi_LightInfoBakingEXT;

// Optional Forward+ sphere light cutoff in scene units, measured from the
// emitter centre. Zero leaves its reach unchanged. Emitter size and falloff
// inside the cutoff are independent.
#define REMIXAPI_STRUCT_TYPE_LIGHT_INFO_FORWARD_CUTOFF_EXT ((remixapi_StructType)0x7fff1074)
typedef struct remixapi_LightInfoForwardCutoffEXT {
  remixapi_StructType sType;
  const void* pNext;
  float distance;
} remixapi_LightInfoForwardCutoffEXT;

// Optional sphere light pNext: the distance from its centre beyond which it
// lights nothing in the path tracer, in scene units (a game light's range, as
// rasterizers give their lights). Its light fades out smoothly towards it, by
// (1 - (d / range)^4)^2: 88% kept at half the range, 12% at 90%, none at it.
// Zero leaves its reach unbounded. Forward+ keeps its own
// (remixapi_LightInfoForwardCutoffEXT). A negative or non-finite range is an
// invalid argument.
#define REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RANGE_EXT ((remixapi_StructType)0x7fff107B)
typedef struct remixapi_LightInfoRangeEXT {
  remixapi_StructType sType;
  const void* pNext;
  float range;
} remixapi_LightInfoRangeEXT;

// Optional pNext on opaque MaterialInfo. Nonzero roughness/metalness constants
// or a roughness/metalness/normal map automatically select EON + GGX. Completely
// zero-initialized legacy parameters retain Lambert shading. This override also
// allows an explicitly smooth (roughness=metalness=0) dielectric.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT ((remixapi_StructType)0x7fff1007)
typedef struct remixapi_MaterialInfoPbrEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool enabled;
} remixapi_MaterialInfoPbrEXT;

// Optional MaterialInfo pNext. A material can omit analytic direct highlights
// in Forward+ while retaining its normal maps and environment reflections.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_DIRECT_SPECULAR_EXT ((remixapi_StructType)0x7fff1077)
typedef struct remixapi_MaterialDirectSpecularEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool enabled;
} remixapi_MaterialDirectSpecularEXT;

// Optional MaterialInfo pNext. Keep the visible emissive color/map while
// suppressing generated area lights, e.g. when an explicit analytic light
// already represents the source. Absent/zero preserves emissive lighting.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT ((remixapi_StructType)0x7fff100a)
typedef struct remixapi_MaterialInfoEmissionEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool suppressLightGeneration;
} remixapi_MaterialInfoEmissionEXT;

// Optional pNext on opaque MaterialInfo: self-illumination, as the "unlit" and
// "self-illuminated" materials of rasterized games. A share of the surface
// shows its own colour instead of reflecting light:
//   m        = maskScale * mask * (emissive texture RGB, or 1 without one)
//   emitted  = emissiveIntensity * emissiveColorConstant * colour * m
//   albedo   = colour * (1 - clamp(mean of m's channels, 0, 1))
// colour is the material's albedo as it would be lit (albedo texture and
// constant, albedo tint, the draw's texture stages and vertex colours; linear).
// mask is one (NONE), the albedo texture's alpha (ALBEDO_ALPHA: self-
// illumination painted into the base texture's alpha channel) or the emissive
// texture's alpha (EMISSIVE_ALPHA; its RGB is then not used). With this
// extension the emissive texture is a mask: its values are used as stored, not
// decoded from sRGB. m above one emits more than the colour (overbright).
// So NONE without an emissive texture is a surface that only emits (unlit),
// and emissiveIntensity * emissiveColorConstant = 1 emits what a white-lit
// Lambertian surface of that colour reflects under uniform radiance 1.
// The emitted light is seen directly and lights the scene like any emissive
// material (MaterialInfoEmissionEXT can suppress the generated lights; their
// power is estimated from the textures and constants, without vertex colours
// or texture stages). Exposure metering and upscaler guides see `colour`.
// Alpha test and blending still use the albedo alpha as without the extension.
// Invalid arguments: a non-opaque material, an unknown mask, maskScale that is
// not finite and positive, EMISSIVE_ALPHA without an emissive texture.
// Path tracing only; the raster backend shows the colour as it always does.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT ((remixapi_StructType)0x7fff100e)
#define REMIXAPI_SELF_ILLUMINATION_MASK_NONE_EXT 0u
#define REMIXAPI_SELF_ILLUMINATION_MASK_ALBEDO_ALPHA_EXT 1u
#define REMIXAPI_SELF_ILLUMINATION_MASK_EMISSIVE_ALPHA_EXT 2u
typedef struct remixapi_MaterialInfoSelfIlluminationEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t mask;   // REMIXAPI_SELF_ILLUMINATION_MASK_*_EXT
  float maskScale; // > 0
} remixapi_MaterialInfoSelfIlluminationEXT;

// Optional InstanceInfo pNext: the instance is a halo source. It is not part
// of the scene (neither traced nor rasterized into the image; no identity,
// motion or lights); the runtime draws its silhouette into a display-
// resolution mask, blurs the mask and composites the blurred colour outside
// the silhouette, as a glow outline (Garry's Mod halo.Add, Source glow
// outlines). Hosts submit the object itself as usual and, in addition, the
// same mesh/transform (and bone transforms) with this extension. Instances
// sharing identical parameters form one halo group (their silhouettes are
// masked together); groups are composited in submission order (of each
// group's first instance), each over the result of the previous ones.
//
// The silhouette is the mesh's coverage, both faces, after its materials'
// alpha test; everything else about the materials is ignored. The halo is
// composited after tonemapping and upscaling, over the world, under view-model
// instances and the UI. Depth-tested halos test against the frame's opaque
// surfaces. SKY and VIEW_MODEL instances use their camera and are never
// depth-tested. At most 32 groups and 4096 mesh surfaces per frame; further
// halo instances are ignored. Non-finite values, blur <= 0 and booleans other
// than 0/1 are invalid arguments; colour is clamped to 0..1, blur to 64 and
// passes to 1..8.
//
// blur matches halo.Add's blur size: the mask is blurred twice by a Gaussian
// whose standard deviation is about two blur sizes. passes is the number of
// times the blurred halo is composited: halo.Add draws it DrawPasses + 1
// times. Additive halos add colour * coverage. Non-additive halos reproduce
// the game's "subtractive" mode, which multiplies the frame by the blurred
// mask (white outside, colour inside): a dark outline for a dark colour, no
// effect for white.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_HALO_EXT ((remixapi_StructType)0x7fff100c)
typedef struct remixapi_InstanceInfoHaloEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Float3D color; // display-referred (sRGB-encoded) 0..1, as a game's UI colour
  float blurX;            // blur extent in pixels at 1080p height (scaled with the output), > 0
  float blurY;
  uint32_t passes;        // the blurred halo is composited this many times (strength), 1..8
  remixapi_Bool additive; // add (glow) or, if false, multiply toward the colour (dark outline)
  remixapi_Bool ignoreZ;  // visible through scene geometry; otherwise depth-tested against it
} remixapi_InstanceInfoHaloEXT;

// Optional InstanceInfo pNext: screen-space masking, what a host did with the
// stencil buffer (a sight's reticle visible only through its lens, a tint
// limited to a scope's opening). Groups are numbered by the host per frame;
// their coverage is independent and lasts for the frame.
//   SOURCE  The instance only marks coverage for its group: it is not part of
//           the scene (as a halo source: neither traced nor rasterized into
//           the image; no identity, motion or lights). Hosts submit the
//           object itself as usual and, in addition, the same mesh/transform
//           (and bone transforms) with this extension. Coverage is its
//           silhouette at display resolution, both faces, after its
//           materials' alpha test (everything else about the materials is
//           ignored), without antialiasing. A group's coverage is the union
//           of its sources. With depthTest the silhouette is tested against
//           the frame's opaque depth (the traced primary surfaces plus
//           unmasked opaque raster-only world draws), biased toward the
//           camera so a source coplanar with its scene instance passes: nearer
//           geometry cuts it; blended surfaces and the view-model layer do
//           not. SKY and VIEW_MODEL instances use their camera and are never
//           depth-tested.
//           `within` (0: none) limits these sources to the coverage of that
//           group as accumulated by the sources submitted before this one
//           (an intersection: stencil increments inside a marked area); a
//           group without sources so far limits it to nothing.
//   INSIDE / OUTSIDE  The instance is drawn as a raster-only overlay, visible
//           only where the group covers / does not cover, whatever its
//           identity classification and blend mode say (never traced or lit;
//           no identity). Its material, InstanceInfoBlendEXT (blend, alpha
//           test, texture stages, write mask), InstanceRasterStateEXT (depth
//           test/write/compare, against the layer's depth as for any
//           raster-only instance), bone transforms, material override and
//           GPU instancing apply as usual. It samples the group's final
//           coverage of the frame: sources may be submitted before or after
//           it. It is sorted with the blended raster-only instances of its
//           layer (after the opaque ones, back to front). While the group has
//           no sources in the frame, INSIDE draws nothing and OUTSIDE draws
//           unmasked. SKY instances cannot be masked and are not drawn.
//   CUT     The instance stays the scene instance it would be without this
//           extension (traced, lit, shadowing, in reflections and indirect
//           light; identity, motion, bone transforms, material, blend and
//           raster state, static or dynamic), except that the camera does not
//           see it where the group covers: primary visibility looks through
//           it to whatever is behind (a scope's body drawn where the stencil
//           is not the eyepiece's value). Opaque, alpha-tested and blended
//           surfaces alike; rays that are not the camera's own (shadows,
//           reflections, bounces) still meet the whole instance. Ray-traced
//           and raster primary visibility both honour it, per rendered sample,
//           so the hole's edge resolves like a geometry edge under an
//           upscaler. This coverage exists before the scene is resolved:
//           it ignores depthTest (of the group's sources and of the sources of
//           any group they are `within`), while INSIDE/OUTSIDE consumers of
//           the same group keep seeing the depth-tested coverage, tested
//           against a depth the cut has already been applied to. Sources may
//           be submitted before or after the instance; a group without
//           sources cuts nothing. At most 8 groups per frame cut (the lowest
//           numbered that have traced CUT instances and sources on screen);
//           CUT instances of further groups are not cut. A replacement mesh
//           (USD mod) of the instance is cut as well. Where the instance is
//           rasterized instead of traced (the raster backend, a raster-only
//           classification, a VIEW_MODEL instance) it is drawn as usual but
//           only outside the coverage, as OUTSIDE (then sorted with its
//           layer's blended instances; the coverage is the depth-tested one).
//           A SKY instance is not cut.
// UI draw commands can be masked the same way through their flags.
// Sources with equal parameters are drawn together, in one pass per run; a
// source starts a new run only when joining an earlier one would change what
// a `within` relation observed. At most 64 runs and 4096 source surfaces per
// frame; further sources are ignored. group outside 1..32, an unknown role,
// within > 32 or equal to group, a depthTest other than 0/1 and a halo
// extension on the same instance are invalid arguments (within and depthTest
// are ignored for INSIDE/OUTSIDE/CUT). Coverage targets and pipelines are
// created when first used; until they are ready, masked draws are left out and
// CUT instances are not cut.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MASK_EXT ((remixapi_StructType)0x7fff100d)
#define REMIXAPI_INSTANCE_MASK_SOURCE_EXT 0u
#define REMIXAPI_INSTANCE_MASK_INSIDE_EXT 1u
#define REMIXAPI_INSTANCE_MASK_OUTSIDE_EXT 2u
#define REMIXAPI_INSTANCE_MASK_CUT_EXT 3u
#define REMIXAPI_INSTANCE_MASK_MAX_GROUP_EXT 32u
typedef struct remixapi_InstanceInfoMaskEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t group;          // 1..REMIXAPI_INSTANCE_MASK_MAX_GROUP_EXT
  uint32_t role;           // REMIXAPI_INSTANCE_MASK_*_EXT
  uint32_t within;         // SOURCE only: 0, or the group limiting this source's coverage
  remixapi_Bool depthTest; // SOURCE only
} remixapi_InstanceInfoMaskEXT;
// remixapi_UIDrawCommand::flags: bits 16..21 hold a mask group (0: unmasked,
// above 32: invalid); the command is drawn only inside its coverage, or only
// outside with OUTSIDE (invalid without a group), as an INSIDE/OUTSIDE instance.
#define REMIXAPI_UI_NATIVE_MASK_GROUP_SHIFT 16u
#define REMIXAPI_UI_NATIVE_MASK_GROUP_BITS 0x3f0000u
#define REMIXAPI_UI_NATIVE_MASK_OUTSIDE 0x400000u

// Optional InstanceInfo pNext: what the camera does not see of a path-traced
// scene instance that every other ray still meets whole. The instance stays
// the instance it would be without this extension (doubleSided, material,
// blend state, identity, classification, shadows, bounces, reflections and
// light sampling unchanged): only rays along the camera's own lines of sight
// skip parts of it, so hiding them changes no lighting. Those rays are the
// primary ray, its straight continuations through blended sheets and painted
// (decal) layers, the previous-frame and confidence re-traces of the same
// lines, the centre-sample depth, and, with raster primary visibility, the
// G-buffer and stable-depth passes. Rays that leave a mirror (primary surface
// replacement), a surface or a light are not camera rays.
//   BACK_FACES  Camera rays skip the instance's back faces: the faces a
//               doubleSided = 0 instance hides from every ray. A triangle
//               (a, b, c) is front-facing to a viewer for whom
//               cross(b - a, c - a) points away, i.e. whose vertices run
//               clockwise seen from the front in a right-handed world (D3D's
//               default front face); a mirroring instance transform swaps the
//               two, as it does for doubleSided = 0. Vertex normals take no
//               part. Without effect on an instance that is not doubleSided.
//   VISIBILITY  Camera rays skip the instance where it lies in a cluster of
//               the visibility partition (remixapi_SetVisibilityPartitionEXT)
//               that is not potentially visible from the camera's cluster.
//               Tested per point: the point a short distance (5 cm, by
//               rtx.native.pathtrace.sceneScale) before the surface along the
//               camera ray is located in the partition; a line of sight that
//               stays inside the visible set therefore never loses a surface,
//               whichever leaves the surface itself spans. Points in leaves
//               without a cluster (solid, outside) are always seen. While the
//               camera's own position has no cluster (outside the world or in
//               solid space), or no partition is set, everything is seen. The
//               visible set of a frame is that of the camera's cluster united
//               with the previous frame's, so last frame's lines of sight
//               stay whole across a cluster boundary.
// Both are evaluated per rendered sample, by ray-traced and raster primary
// visibility alike. flags with unknown bits are invalid arguments. Halo and
// mask sources and INSIDE/OUTSIDE overlays ignore the extension, as do
// instances that are rasterized instead of traced (the raster backend, a
// raster-only classification, SKY and VIEW_MODEL categories). A USD mod's
// replacement of the instance keeps VISIBILITY and drops BACK_FACES (its
// meshes carry their own winding). rtx.native.pathtrace.cameraBackFaceCulling
// and cameraVisibilityCulling (default 1) switch each off for every instance.
// A frame without such instances costs nothing; one with them traces its
// camera rays without the opaque-geometry shortcut.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_CAMERA_CULLING_EXT ((remixapi_StructType)0x7fff100f)
#define REMIXAPI_INSTANCE_CAMERA_CULL_BACK_FACES_EXT 1u
#define REMIXAPI_INSTANCE_CAMERA_CULL_VISIBILITY_EXT 2u
typedef struct remixapi_InstanceInfoCameraCullingEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t flags; // REMIXAPI_INSTANCE_CAMERA_CULL_*_EXT
} remixapi_InstanceInfoCameraCullingEXT;

// Optional InstanceInfo pNext: only camera rays meet the instance (with the
// rays a camera sends on through ray portals and screens); shadow, light,
// bounce and reflection rays pass it, as they pass a ray portal's opening. A
// backdrop drawn as geometry, such as a skybox mesh around a game's 3D-skybox
// diorama, then shows without shutting the sun and sky out of what it
// surrounds, or lighting it. Path-traced instances only: the raster backend
// draws the instance as usual.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_CAMERA_ONLY_EXT ((remixapi_StructType)0x7fff1077)
typedef struct remixapi_InstanceInfoCameraOnlyEXT {
  remixapi_StructType sType;
  const void *pNext;
} remixapi_InstanceInfoCameraOnlyEXT;

// Optional InstanceInfo pNext: a backdrop the camera sees behind the world, as
// a game draws a 3D skybox first and its world over it, whatever lies where
// once the skybox is placed around the world at the world's scale. Three
// roles:
//   PORTAL    An opening onto the backdrop (a sky brush's face): no ray meets
//             it. A camera ray that crosses its front face (the face a
//             doubleSided = 0 instance shows) before any surface of the world
//             shows the nearest GEOMETRY or BEHIND surface along its whole
//             line instead, in front of the opening or past it, or the sky
//             where there is none: what lies past the opening that is not
//             backdrop (the rest of the world, which the game's sky brush
//             hides) is not seen.
//   GEOMETRY  Backdrop that is in the scene like any instance (it lies where
//             the world is not): every ray meets it.
//   BEHIND    Backdrop that lies inside the world's own space (the skybox's
//             ground crossing a room): met by camera rays that crossed a
//             PORTAL only, as above; no other ray meets it (it casts no
//             shadow and lights nothing).
// Camera rays are those of remixapi_InstanceInfoCameraCullingEXT. The line of
// sight past a mirror, through a screen or a ray portal, and bounce, shadow
// and light rays take no part: they meet GEOMETRY only. Path-traced
// instances; the raster and Forward+ backends draw GEOMETRY as usual and
// leave PORTAL and BEHIND instances out. An unknown role is an invalid
// argument. A runtime without the extension would draw PORTAL and BEHIND
// instances as geometry: a host sends them only where the runtime exports
// remixapi_InstanceBackdropSupportedEXT (which returns SUCCESS).
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BACKDROP_EXT ((remixapi_StructType)0x7fff1078)
#define REMIXAPI_INSTANCE_BACKDROP_GEOMETRY_EXT 1u
#define REMIXAPI_INSTANCE_BACKDROP_BEHIND_EXT 2u
#define REMIXAPI_INSTANCE_BACKDROP_PORTAL_EXT 3u
typedef struct remixapi_InstanceInfoBackdropEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t role; // REMIXAPI_INSTANCE_BACKDROP_*_EXT
} remixapi_InstanceInfoBackdropEXT;

// Optional MaterialInfo pNext (after its Opaque struct) on a material that
// shows another view: a screen (remixapi_MaterialInfoScreenCameraEXT) or a
// ray portal over an opaque surface. What it shows takes the effects of a
// game's screen shader (Source's MonitorScreen, a Unity screen material):
//  tint: multiplies the view, each channel 0..1 ({1, 1, 1}: none).
//  saturation: mix(mean(c), c, saturation), the mean of the three channels
//   (1: none, 0: grey; MonitorScreen's $saturation).
//  contrast: mix(c, c * c, contrast) (0: none; MonitorScreen's $contrast).
//  chromaticAberration: the red channel displaced right and the blue channel
//   left of the green by this fraction of the output's width (0: none, at
//   most 1/240); a neighbouring pixel lends its channel only where it shows
//   such a view too.
// Saturation and contrast apply to the displayed (SDR-encoded) colour, as a
// game's shader sees its camera's image; with an HDR output, to the exposed
// linear colour. Tint applies in the view's lighting, saturation, contrast and
// the aberration to the final image (after upscaling).
#define REMIXAPI_STRUCT_TYPE_MATERIAL_VIEW_EFFECTS_EXT ((remixapi_StructType)0x7fff107A)
typedef struct remixapi_MaterialInfoViewEffectsEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Float3D tint;
  float saturation;
  float contrast;
  float chromaticAberration;
} remixapi_MaterialInfoViewEffectsEXT;

// Optional TextureInfo pNext. With `generate` set and a single supplied level
// (mipLevels = 1), the runtime completes the texture with a box-filtered mip
// chain (sRGB colors averaged in linear space). For hosts that capture only a
// texture's top level, e.g. D3D9 proxies: without mips, traced surfaces sample
// the full-resolution level at any distance and alias. Leave it unset for
// textures that are single-level by design (lookup tables, UI, gradients).
#define REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT ((remixapi_StructType)0x7fff100b)
typedef struct remixapi_TextureInfoGenerateMipsEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool generate;
} remixapi_TextureInfoGenerateMipsEXT;

// Optional TextureInfo pNext: a texture whose pixels the host rewrites with
// remixapi_UpdateTextureEXT (a render target the game redraws, a video, a
// screen). The texture is one object for its whole life: updates change its
// pixels, never its identity, so the materials made from it, their descriptor
// slots, and everything the runtime keys by texture or material stay as they
// are, and an update costs one copy of the changed rows and one staged upload
// of them. What the runtime derives from a texture's contents on the CPU is
// defined for a dynamic one as follows:
//   - Emissive light estimates (the triangle lights of emissive and
//     self-illuminated materials that sample it) follow the pixels at a low
//     rate: once the pixels changed, a material's estimates are made again
//     when they are REMIXAPI_DYNAMIC_TEXTURE_EMISSION_SECONDS_EXT old (older
//     when they are expensive: making them takes under 1% of the time between
//     two). Until then the lights keep the previous image's power, also on
//     meshes the host creates anew every frame. What the camera and rays see
//     of the surface itself is the current image, every frame. An instance
//     of the persistent static set (retained instances) keeps the estimate
//     made when it joined the set.
//   - Opacity: every texel counts as possibly transparent, whatever the pixels
//     are. Alpha-tested surfaces are tested per ray; none is ever proven
//     opaque from a past image.
//   - It has no content hash: the scene counts as changed whenever one of its
//     dynamic textures was updated (accumulation restarts, a still frame is
//     rendered again).
// Requirements (INVALID_ARGUMENTS otherwise): dynamic is 0 or 1; with 1, the
// format is R8G8B8A8 or B8G8R8A8 (UNORM or SRGB) or a float one
// (REMIXAPI_FORMAT_R16G16B16A16_SFLOAT_EXT, _R32G32B32A32_SFLOAT_EXT: an HDR
// render target such as a dome's sky, updated in float), mipLevels is 1 and
// remixapi_TextureInfoGenerateMipsEXT is absent or off: a dynamic texture has
// one level. CreateTexture under the hash of an existing texture makes a new
// texture, as it always does: materials created before keep the old one.
#define REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT ((remixapi_StructType)0x7fff1016)
#define REMIXAPI_DYNAMIC_TEXTURE_EMISSION_SECONDS_EXT 0.25
typedef struct remixapi_TextureInfoDynamicEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool dynamic;
} remixapi_TextureInfoDynamicEXT;

// Optional MeshInfo pNext. An opt-in replacement of the same mesh hash may
// reuse update-capable geometry when all surface vertex counts and index values
// match. DestroyMesh ends that lifetime. This is a storage hint, not object or
// temporal identity. Queued older meshes retain their immutable contents.
// Bone-skinned meshes continue to use the independent per-instance skin path.
#define REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT ((remixapi_StructType)0x7fff1008)
typedef struct remixapi_MeshInfoUpdateEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool enabled;
} remixapi_MeshInfoUpdateEXT;

// Optional MeshInfo pNext, alongside enabled MeshInfoUpdateEXT. The caller
// guarantees that a vertex index denotes the same logical point across mesh
// replacements. With stable InstanceIdentityEXT, compatible replacements may
// retain motion correspondence to the last GPU-submitted mesh. Exact topology
// is still checked; DestroyMesh, index/count changes or disabling either hint
// ends correspondence. Ambiguous/recycled object identities must not opt in.
#define REMIXAPI_STRUCT_TYPE_MESH_MOTION_EXT ((remixapi_StructType)0x7fff1009)
typedef struct remixapi_MeshInfoMotionEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool enabled;
} remixapi_MeshInfoMotionEXT;

// Optional InstanceInfo pNext. The hash a USD mod keys this draw's mesh
// replacement by (mesh_<hash>) when it is not the mesh handle, e.g. for a
// translation layer reproducing the capture runtime's geometry hashes.
// Zero keeps the mesh handle as the key.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_REPLACEMENT_HASH_EXT ((remixapi_StructType)0x7fff1012)
typedef struct remixapi_InstanceInfoReplacementHashEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint64_t meshHash;
} remixapi_InstanceInfoReplacementHashEXT;

// Optional MaterialInfo pNext (after its Opaque/Translucent struct). The hash
// a USD mod keys this material's replacement by (mat_<hash>) when it is not
// the material's own hash, e.g. for a translation layer whose material stands
// in for one the capture runtime keyed by another texture. Zero keeps the
// material's hash as the key.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_REPLACEMENT_HASH_EXT ((remixapi_StructType)0x7fff1017)
typedef struct remixapi_MaterialInfoReplacementHashEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint64_t materialHash;
} remixapi_MaterialInfoReplacementHashEXT;

// Optional alternate USD material replacement keys, in preference order after
// the material's own hash and MaterialInfoReplacementHashEXT, before its albedo
// texture hash. CreateMaterial copies the array. At most 32 entries; zero keys
// and duplicates are ignored. A null array is valid only when count is zero.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_REPLACEMENT_KEYS_EXT ((remixapi_StructType)0x7fff1076)
typedef struct remixapi_MaterialReplacementKeysEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t count;
  const uint64_t *keys;
} remixapi_MaterialReplacementKeysEXT;

// Raw BC1 uploads with one-bit alpha. The retained BC1_RGB formats remain opaque.
#define REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT ((remixapi_Format)133)
#define REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT ((remixapi_Format)134)

// Additional UIDrawCommand flags supported by this native raster runtime. Defaults
// retain the original clamp/nearest/alpha-blend behavior and the 0.1003 layout.
#define REMIXAPI_UI_NATIVE_REPEAT_U 0x2u
#define REMIXAPI_UI_NATIVE_REPEAT_V 0x4u
#define REMIXAPI_UI_NATIVE_LINEAR 0x8u
#define REMIXAPI_UI_NATIVE_CUSTOM_BLEND 0x10u
// With CUSTOM_BLEND, bits 8..11 and 12..15 hold source/destination Vulkan factors (0..14).
#define REMIXAPI_UI_NATIVE_SRC_BLEND_SHIFT 8u
#define REMIXAPI_UI_NATIVE_DST_BLEND_SHIFT 12u

// Optional pNext on InstanceInfo. Depth comparison uses Vulkan's 0..7 compare values.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT ((remixapi_StructType)0x7fff1001)
typedef struct remixapi_InstanceRasterStateEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool depthTest;
  remixapi_Bool depthWrite;
  uint32_t depthCompare;
  remixapi_Bool disableColorWrites; /* depth-only prepass; zero preserves color writes */
} remixapi_InstanceRasterStateEXT;

// View models (REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL instances, with a
// REMIXAPI_CAMERA_TYPE_VIEW_MODEL camera) are path-traced when
// rtx.native.pathtrace.viewModel is on (off: rasterized with their camera):
// placed in the world camera's view so that it projects them where their own
// camera would (its field of view, the world camera's depth range), shrunk
// toward the eye by rtx.native.pathtrace.viewModelScale (the same image,
// nearer). The camera sees them in front of everything, whatever reaches
// nearer (raster and ray-traced primary visibility alike); shadow, bounce and
// reflection rays pass them, except those of a path that starts on one (they
// shadow themselves, not the world); their emission lights nothing. Static
// classifications are traced as dynamic. Raster-only view-model instances
// are drawn in the same place with the world camera, depth-tested against
// the traced frame. Needs perspective world and view-model cameras.
// Optional InstanceInfo pNext: the instance is drawn as the edges of its
// triangles (D3D's D3DFILL_WIREFRAME): a raster-only overlay of one-pixel
// lines whatever its identity classification and blend mode say (never
// traced, lit or shadowing; no identity), coloured as any raster-only
// instance (material x vertex colour through InstanceInfoBlendEXT's texture
// stages and tFactor) and depth-tested as InstanceRasterStateEXT says against
// the frame's depth. Single-sided instances lose their back faces' edges, as
// with culling. A device without non-solid fill (fillModeNonSolid) draws the
// triangles filled. flags must be 0.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_WIREFRAME_EXT ((remixapi_StructType)0x7fff101d)
typedef struct remixapi_InstanceInfoWireframeEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t flags;
} remixapi_InstanceInfoWireframeEXT;

// Stable identity is scoped to the caller's world/object lifetime. GPU instancing
// adds the array index; each mesh surface retains its geometry index. Zero IDs are
// reserved for legacy frame-local draws. rasterVisible does not affect tracing.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT ((remixapi_StructType)0x7fff1002)
typedef enum remixapi_InstanceClassEXT {
  REMIXAPI_INSTANCE_STATIC_EXT = 0,
  REMIXAPI_INSTANCE_DYNAMIC_EXT = 1,
  REMIXAPI_INSTANCE_RASTER_ONLY_EXT = 2,
} remixapi_InstanceClassEXT;
typedef struct remixapi_InstanceIdentityEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint64_t instanceId;
  uint32_t classification;
  remixapi_Bool rasterVisible;
} remixapi_InstanceIdentityEXT;

// Optional InstanceInfo pNext on a blended raster-only instance
// (classification REMIXAPI_INSTANCE_RASTER_ONLY_EXT): its place among the
// blended raster-only draws of its layer is the host's, not its depth. A
// rasterizing host already drew its translucent effects in the order that
// composes them (particles: back to front per system, a system's smoke before
// the fire inside it): the centres of their per-frame meshes say little, and
// sorting by them swaps layers as the camera moves. Instances with a nonzero
// `order` are drawn after their layer's depth-sorted blended draws, in
// ascending order (equal values: in submission order). Zero, an opaque or a
// traced instance: ignored. Masked overlays (remixapi_InstanceInfoMaskEXT) keep
// their depth-sorted place.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_RASTER_ORDER_EXT ((remixapi_StructType)0x7fff1013)
typedef struct remixapi_InstanceInfoRasterOrderEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t order;
} remixapi_InstanceInfoRasterOrderEXT;

// Sprite overlays: what a rasterizing host's particle shaders do beyond
// texture x vertex colour, for blended raster-only instances (classification
// REMIXAPI_INSTANCE_RASTER_ONLY_EXT, not masked). Other instances ignore both
// structures.
//
// Optional MeshInfo pNext. With `enabled`, the vertices of every surface carry
// sprite parameters where a lit mesh has normals and padding (such a mesh has
// no normals: it is for raster-only instances):
//   normal            (u1, v1, weight1): the second frame of the sprite sheet
//                     and its weight
//   _pad0 .. _pad4    as floats (u2, v2, u3, v3, weight2): the two frames of a
//                     second sequence and the second one's weight
// Values must be finite. They are interpolated like texture coordinates.
#define REMIXAPI_STRUCT_TYPE_MESH_SPRITE_VERTICES_EXT ((remixapi_StructType)0x7fff1014)
typedef struct remixapi_MeshInfoSpriteVerticesEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool enabled;
} remixapi_MeshInfoSpriteVerticesEXT;

// Optional InstanceInfo pNext. T(uv) is the material's albedo texel (decoded
// to linear), lum(c) = dot(c, (0.3, 0.59, 0.11)). In this order:
//   FRAME_BLEND          texel = mix(T(uv), T(uv1), weight1)
//   FRAME_MAX_LUMINANCE  texel = lum(T(uv1) * weight1) >= lum(T(uv) * (1 -
//                        weight1)) ? T(uv1) : T(uv)   (instead of the blend)
//   GREEN_ALPHA          the green and alpha channels of the two frames are
//                        four alpha-only images, cross-faded by weight1 (w):
//                        with k = w < 1/4 ? (1/2 - 2w, 2w + 1/2, 0, 0)
//                               : w < 3/4 ? (0, 3/2 - 2w, 2w - 1/2, 0)
//                                         : (0, 0, 5/2 - 2w, 2w - 3/2),
//                        a = T(uv).r w + T(uv).g k.x + T(uv).a k.y + T(uv1).g
//                        k.z + T(uv1).a k.w and texel = (a, a, a, a)
//                        (Source's $extractgreenalpha, its red term included).
//   SEQUENCE_*           a second sequence s = mix(T(uv2), T(uv3), weight2)
//                        (with SEQUENCE_MAX_LUMINANCE the brighter of the two
//                        as above, weighted by weight1 as Source does):
//                        ADD      texel = (texel + s) / 2
//                        REPLACE  texel.rgb = s.rgb
//                        OVER     texel.rgb = mix(texel.rgb, s.rgb, s.a)
// The texel then goes through the instance's texture stages (texture x vertex
// colour, ...) and alpha test as usual, and
//   SOFT_DEPTH           alpha *= saturate(max(smoothstep(softFadeStart,
//                        softFadeEnd, scene), |scene - fragment| /
//                        softDistance)): the sprite fades out where it meets
//                        the scene. `scene` is the view-space depth of the
//                        frame's opaque surfaces (the traced primary surfaces)
//                        at the pixel, `fragment` the sprite's, in world
//                        units; softFadeEnd <= softFadeStart leaves the first
//                        term out. Ignored (a hard intersection, as without
//                        the flag) while the scene is not traced, and on SKY
//                        and VIEW_MODEL instances.
//   MODULATE             for blend factors DST_COLOR / SRC_COLOR (k = 2) or
//                        DST_COLOR / ZERO, ZERO / SRC_COLOR (k = 1): the
//                        product dst x src x k is taken on display-encoded
//                        values, as hosts that blend into an 8-bit frame
//                        buffer define it (mid grey is neutral for k = 2),
//                        whatever space the target blends in. src = T(uv).rgb
//                        (display-encoded) x vertex colour, texture stages
//                        aside; a target that blends linear values cannot
//                        brighten by more than k.
//   MODULATE_FADE        with MODULATE: src = mix(1 / k, src, alpha), the
//                        sprite fading to no effect with its alpha.
// softDistance must be positive with SOFT_DEPTH. The frame and sequence flags
// need a mesh with sprite vertices and are ignored without.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_SPRITE_EXT ((remixapi_StructType)0x7fff1015)
#define REMIXAPI_SPRITE_FRAME_BLEND_EXT 0x1u
#define REMIXAPI_SPRITE_FRAME_MAX_LUMINANCE_EXT 0x2u
#define REMIXAPI_SPRITE_GREEN_ALPHA_EXT 0x4u
#define REMIXAPI_SPRITE_SEQUENCE_ADD_EXT 0x8u
#define REMIXAPI_SPRITE_SEQUENCE_REPLACE_EXT 0x10u
#define REMIXAPI_SPRITE_SEQUENCE_OVER_EXT 0x18u
#define REMIXAPI_SPRITE_SEQUENCE_MAX_LUMINANCE_EXT 0x20u
#define REMIXAPI_SPRITE_SOFT_DEPTH_EXT 0x40u
#define REMIXAPI_SPRITE_MODULATE_EXT 0x80u
#define REMIXAPI_SPRITE_MODULATE_FADE_EXT 0x100u
typedef struct remixapi_InstanceInfoSpriteEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t flags;
  float softDistance;
  float softFadeStart;
  float softFadeEnd;
} remixapi_InstanceInfoSpriteEXT;

typedef enum remixapi_RenderBackendEXT {
  REMIXAPI_RENDER_BACKEND_RASTER_EXT = 0,
  REMIXAPI_RENDER_BACKEND_PATHTRACE_EXT = 1,
} remixapi_RenderBackendEXT;
// Versioned independent exports: initialize structSize to sizeof(the struct).
// Missing capabilities are reported as a human-readable string; no RTX legacy
// feature bits, denoising or upscaling are implied by this capability.
typedef struct remixapi_BackendCapabilitiesEXT {
  uint32_t structSize;
  uint32_t supportedBackends; // bit 0 raster, bit 1 pathtrace, bit 2 forwardplus
  uint32_t activeBackend;
  uint32_t requestedBackend;
  char unavailableReason[256];
} remixapi_BackendCapabilitiesEXT;

// Independent preparation diagnostics. An active backend can still compile
// scene variants or prepare requested settings. Reference captures should wait
// for pending/loading to clear as well as their backend/bake readiness checks.
// Pipeline counters are cumulative since Startup. This export does not wait for
// compilation or GPU completion; continue presenting normally while pending.
typedef struct remixapi_PreparationStatisticsEXT {
  uint32_t structSize, pending, loading, failed;
  uint64_t pipelinesRequested, pipelinesCompleted, pipelinesFailed;
  char failureReason[256];
} remixapi_PreparationStatisticsEXT;
typedef enum remixapi_RebuildReasonEXT {
  REMIXAPI_REBUILD_NONE_EXT = 0,
  REMIXAPI_REBUILD_MEMBERSHIP_EXT = 1,
  REMIXAPI_REBUILD_GEOMETRY_EXT = 2,
  REMIXAPI_REBUILD_REFIT_LIMIT_EXT = 3,
  REMIXAPI_REBUILD_DISPLACEMENT_EXT = 4,
  REMIXAPI_REBUILD_FORCED_EXT = 5,
  REMIXAPI_REBUILD_STATIC_TRANSFORM_EXT = 6,
} remixapi_RebuildReasonEXT;
// Optional SHARC diagnostics; counters/times describe the last completed cache frame.
// Query counters sample 1/64 pixels (excluding confidence retracing), not all rays.
// Optional sun-shadow diagnostics. Existing denoising timing remains diffuse SH only.
typedef struct remixapi_ShadowStatisticsEXT {
  uint32_t structSize, supported, requested, active;
  uint32_t gpuTimingsAvailable, debugView, reserved[2];
  uint64_t renderedFrames, historyResets;
  float gpuSigmaMilliseconds, sunAngularDiameterDegrees;
  char unavailableReason[256];
} remixapi_ShadowStatisticsEXT;

// Analytic local emitters and their persistent importance hierarchy. Sampling cost
// is included in tracing time; gpuDenoiseMilliseconds measures only local NRD.
typedef struct remixapi_LocalLightStatisticsEXT {
  uint32_t structSize, enabled, lightCount, activeLights;
  uint32_t nodeCount, treeDepth, samples, denoiserActive;
  uint32_t gpuTimingsAvailable, reserved;
  uint64_t generation, builds, refits, memoryBytes;
  double cpuUpdateMilliseconds, gpuDenoiseMilliseconds;
  char unavailableReason[256];
} remixapi_LocalLightStatisticsEXT;

// The local light hierarchy's two parts and what keeping it up to date costs.
// The static part holds the lights that were present and at rest when the
// hierarchy was built: their emitter and node indices stay put until the next
// build (a light that leaves keeps its slot, unlit: retiredSlots). Lights
// that appear afterwards, and the emitters of instances submitted as
// REMIXAPI_INSTANCE_DYNAMIC_EXT with an instanceId, form the dynamic part,
// which is rebuilt alone (dynamicBuilds, counted in refits) whenever its
// membership changes; it is visible from every cluster of a visibility
// partition. Only a build (builds, by reason below) changes static indices,
// remakes the visibility table and resets the guiding history. Lights that
// wait in the dynamic part without belonging to a dynamic instance join the
// static part once at rest (SETTLED), at most one build every 120 frames.
#define REMIXAPI_LOCAL_LIGHT_BUILD_NONE_EXT 0u
#define REMIXAPI_LOCAL_LIGHT_BUILD_INITIAL_EXT 1u       // first lights
#define REMIXAPI_LOCAL_LIGHT_BUILD_FIRST_DYNAMIC_EXT 2u // the hierarchy gained its dynamic part
#define REMIXAPI_LOCAL_LIGHT_BUILD_SETTLED_EXT 3u       // lights at rest joined the static part
#define REMIXAPI_LOCAL_LIGHT_BUILD_RETIRED_EXT 4u       // most static slots had lost their light
#define REMIXAPI_LOCAL_LIGHT_BUILD_CLEARED_EXT 5u       // no lights left
#define REMIXAPI_LOCAL_LIGHT_BUILD_REASONS_EXT 6u
typedef struct remixapi_LocalLightSetStatisticsEXT {
  uint32_t structSize, lastBuildReason;
  // Lights in static slots, lights in the dynamic part, static slots without
  // their light, and emitters of dynamic instances (all in the dynamic part).
  uint32_t staticLights, dynamicLights, retiredSlots, dynamicInstanceLights;
  // Instances (outside the static set) with emitters this frame, and those
  // among them identified as dynamic: more than the scene has glowing props
  // means the same prop was submitted under several identities.
  uint32_t emittingInstances, dynamicEmittingInstances;
  uint64_t builds, refits, dynamicBuilds;
  uint64_t buildsByReason[REMIXAPI_LOCAL_LIGHT_BUILD_REASONS_EXT];
  // Visibility table (remixapi_VisibilityPartitionEXT): times made and
  // uploaded, its size, light-tree nodes it covers.
  uint64_t tableBuilds, tableUploads, tableBytes, tableNodes;
  // CPU milliseconds: last table made, all tables; last hierarchy build, all
  // builds; the frame's whole light update (hierarchies and table), and the
  // longest of the last 300 to 600 frames.
  double tableMilliseconds, tableTotalMilliseconds;
  double buildMilliseconds, buildTotalMilliseconds;
  double updateMilliseconds, updatePeakMilliseconds;
} remixapi_LocalLightSetStatisticsEXT;

// remixapi_InstanceInfoCameraCullingEXT in the last traced frame. Clusters and
// leaves are those of the visibility partition; -1: no cluster.
typedef struct remixapi_CameraCullingStatisticsEXT {
  uint32_t structSize;
  // Bits of REMIXAPI_INSTANCE_CAMERA_CULL_*_EXT: asked for by traced instances
  // of the frame, and applied to its camera rays (a bit asked for but not
  // applied: switched off, or VISIBILITY without a partition or with the
  // camera outside every cluster).
  uint32_t requested, active;
  // The partition is set; the camera (this frame's or the previous one's) is
  // in a leaf without a cluster, so everything is seen.
  uint32_t partition, outside;
  int32_t cameraCluster, previousCluster;
  uint32_t clusterCount, visibleClusters; // visible: in the frame's united set
  uint32_t leafCount, visibleLeaves;      // leaves without a cluster count as visible
  uint32_t reserved;
  uint64_t updates; // times the visible set changed (camera crossed a cluster boundary)
} remixapi_CameraCullingStatisticsEXT;

// Optional visibility-guided local-light diagnostics. Query counters are sampled
// at 1/64 of opaque-primary light samples; they are not full ray counts.
typedef struct remixapi_LocalLightGuidingStatisticsEXT {
  uint32_t structSize, requested, active, capacity;
  uint32_t sampledQueries, sampledGuided, sampledVisible, reserved;
  uint64_t renderedFrames, historyResets, memoryBytes;
  float influenceRange, cellSize;
  char unavailableReason[256];
} remixapi_LocalLightGuidingStatisticsEXT;

typedef struct remixapi_SharcStatisticsEXT {
  uint32_t structSize, supported, requested, active;
  uint32_t capacity, occupiedEntries, updatePaths, queryAttempts, queryHits;
  uint32_t gpuTimingsAvailable, debugView, reserved;
  uint64_t memoryBytes, renderedFrames, resets;
  double gpuUpdateMilliseconds, gpuResolveMilliseconds;
  char unavailableReason[256];
} remixapi_SharcStatisticsEXT;

typedef struct remixapi_TracingStatisticsEXT {
  uint32_t structSize, samplesPerPixel, indirectBounces, accumulationEnabled;
  uint64_t accumulationCount, sampleSequence, sceneGeneration;
  uint64_t blasBuilds, blasRefits, blasCompactions;
  uint64_t staticTlasBuilds, dynamicTlasBuilds, dynamicTlasRefits;
  uint64_t accelerationBytes, peakAccelerationBytes;
  uint32_t dynamicRebuildReason, dynamicRefitCount;
  float gpuBuildMilliseconds, gpuTraceMilliseconds;
  uint32_t gpuTimingsAvailable, redrawRequested;
} remixapi_TracingStatisticsEXT;

// CPU wall times for the last Present attempt. Geometry, allocation, upload and
// acceleration times are nested within command recording; do not sum them with
// recordMilliseconds. GPU execution is measured by TracingStatisticsEXT instead.
// attempts/presentations are cumulative since Startup; presented excludes skips.
typedef struct remixapi_FramePerformanceEXT {
  uint32_t structSize, presented;
  uint64_t attempts, presentations;
  double presentMilliseconds, sceneMilliseconds, recordMilliseconds;
  double geometryMilliseconds, allocationMilliseconds, uploadMilliseconds;
  double accelerationMilliseconds, fenceMilliseconds, acquireMilliseconds;
  double submitMilliseconds, queuePresentMilliseconds;
  uint64_t geometryConversions, geometryCacheHits, uploadBytes;
  uint64_t bufferAllocations, allocationBytes, bufferCacheHits;
  uint64_t memoryAllocations, memoryAllocationBytes;
} remixapi_FramePerformanceEXT;

// CPU wall-time breakdown of the last Present attempt's command recording.
// Every phase up to otherMilliseconds is nested within
// FramePerformanceEXT.recordMilliseconds; the named sub-phases (skinning,
// material, temporal) are nested within their parent. FramePerformanceEXT
// geometry time is nested within traceSurfaces and its acceleration time within
// traceAcceleration. Within traceSurfaces, material is the draw-order material
// slot assignment and temporal the history staging; per-surface table filling
// (including history lookups) runs on worker threads and is not split out.
// otherMilliseconds is the part of recordMilliseconds that no phase covers. presentSetup (settings, pipelines, frame
// slot selection) and presentRelease (history commit, snapshot and draw list release) are the parts of
// presentMilliseconds outside scene, fence, acquire, record, submit and queue present.
typedef struct remixapi_FrameBreakdownEXT {
  uint32_t structSize, reserved;
  double rasterPrepareMilliseconds, rasterSkinningMilliseconds;
  double traceBeginMilliseconds, traceSetupMilliseconds;
  double traceSurfacesMilliseconds, materialMilliseconds, temporalMilliseconds;
  double retireMilliseconds, traceAccelerationMilliseconds, tableMilliseconds;
  double lightingMilliseconds, sharcMilliseconds, signatureMilliseconds;
  double descriptorMilliseconds, primaryMilliseconds, passesMilliseconds;
  double postMilliseconds, compositeMilliseconds, uiMilliseconds;
  double snapshotMilliseconds, otherMilliseconds;
  double presentSetupMilliseconds, presentReleaseMilliseconds;
  uint64_t traceInstances, traceSurfaces, primaryDraws, rasterDraws;
  // rasterCulledDraws (was reserved, zero): raster draws outside the view,
  // left out before they were recorded; rasterDraws counts them too.
  uint64_t textureDescriptors, rasterSkinnedVertices, rasterUploadBytes, rasterCulledDraws;
} remixapi_FrameBreakdownEXT;

// GPU pass timings of the newest completed frame, while the
// "rtx.native.gpuPassTimers" setting is on (available is 0 otherwise). A frame
// is one command buffer on one queue (queueFamily). Every pass is a range of
// two timestamps written at the bottom of the pipe: its time is what the
// queue took to finish the pass after the work recorded before it, so passes
// do not overlap. Passes may nest (parent, depth); selfMilliseconds excludes
// nested passes. frameMilliseconds spans the whole command buffer and equals
// passMilliseconds (top-level passes) plus untimedMilliseconds (barriers and
// commands outside any pass). width/height are the pass's dispatch or render
// resolution (0 when it has none). Passes are in recording order; truncated
// counts passes that did not fit. Results trail the recorded frame by the
// frame-slot count; reading them never waits for the GPU.
#define REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT 128
typedef struct remixapi_GpuPassTimingEXT {
  char name[56];
  uint32_t width, height;
  uint32_t depth, parent; // parent: index of the enclosing pass, or UINT32_MAX
  double milliseconds, selfMilliseconds;
} remixapi_GpuPassTimingEXT;
typedef struct remixapi_GpuPassTimingsEXT {
  uint32_t structSize, available;
  uint32_t passCount, truncated;
  uint32_t queueFamily, timestampValidBits;
  uint64_t frame; // frames recorded since Startup, at the measured frame
  double frameMilliseconds, passMilliseconds, untimedMilliseconds;
  double timestampPeriod; // nanoseconds per tick
  uint32_t renderWidth, renderHeight, outputWidth, outputHeight;
  remixapi_GpuPassTimingEXT passes[REMIXAPI_GPU_PASS_TIMING_CAPACITY_EXT];
} remixapi_GpuPassTimingsEXT;

// Optional CameraInfo pNext. Epochs persist in every packet, including replacement
// submissions. Increment historyResetEpoch for cuts and floating-origin rebases.
#define REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT ((remixapi_StructType)0x7fff1003)
typedef struct remixapi_CameraTemporalEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint64_t worldEpoch, historyResetEpoch;
} remixapi_CameraTemporalEXT;
// Optional world CameraInfo pNext. Linear, nonnegative, finite RGB radiance at
// unit strength; enabled must be 0 or 1. Absent/disabled means black indirect
// environment. Sky/viewmodel cameras never replace the world environment.
#define REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT ((remixapi_StructType)0x7fff1004)
typedef struct remixapi_CameraEnvironmentEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool enabled;
  remixapi_Float3D radiance;
} remixapi_CameraEnvironmentEXT;

// Optional pNext on a world CameraInfo: the camera is inside a translucent
// body (under water). What it sees is absorbed as through such a body
// (remixapi_MaterialInfoTranslucentEXT without a thin wall: the colour to the
// power of the distance over the measurement distance) up to the first
// surface, and what is absorbed becomes the body's colour lit by the sky and
// the sun. Absent, or inside 0: the camera is in air.
#define REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_EXT ((remixapi_StructType)0x7fff101c)
typedef struct remixapi_CameraMediumEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Bool inside;
  remixapi_Float3D transmittanceColor;
  float transmittanceMeasurementDistance;
} remixapi_CameraMediumEXT;

// Optional pNext on a world CameraInfo: this frame's screen cameras, the
// cameras that surfaces with remixapi_MaterialInfoScreenCameraEXT show (a
// security monitor, a game's render-to-texture view). A surface of camera
// `slot` shows, at texture coordinate (u, v), what the camera sees through the
// point (2u - 1, 1 - 2v) of its image plane: u to the right, v down, the image
// covering [0, 1] in both and repeating outside it. The primary surface
// replacement pass traces it as the pixel's surface (lit, shadowed and
// denoised as a surface in view), so it needs a raster primary
// (rtx.native.pathtrace.primaryVisibility) and a denoiser; elsewhere, and for
// a slot not given this frame, the screen shows its material. A screen camera
// also sees reflection-only instances (the third-person player categories), so
// what only it should see is submitted that way; reflection-only paint coats
// only reflection-only surfaces. Slots are 0 to
// REMIXAPI_SCREEN_CAMERA_SLOTS_EXT - 1; directions need not be unit length but
// must not be parallel; fovY in degrees (0, 180), aspect width / height.
#define REMIXAPI_STRUCT_TYPE_CAMERA_SCREENS_EXT ((remixapi_StructType)0x7fff1021)
#define REMIXAPI_SCREEN_CAMERA_SLOTS_EXT 7u
typedef struct remixapi_ScreenCameraEXT {
  uint32_t slot;
  remixapi_Float3D position, forward, up, right;
  float fovYDegrees, aspect;
} remixapi_ScreenCameraEXT;
typedef struct remixapi_CameraScreensEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t count;
  const remixapi_ScreenCameraEXT *cameras;
} remixapi_CameraScreensEXT;

// Optional pNext on an opaque MaterialInfo: the surface is a screen showing
// screen camera `slot` (remixapi_CameraScreensEXT), mapped by its texture
// coordinates. The material's own albedo and textures show where the camera
// is not traced (rays reaching the screen after a bounce, a slot not set).
#define REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_SCREEN_CAMERA_EXT ((remixapi_StructType)0x7fff1022)
typedef struct remixapi_MaterialInfoScreenCameraEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t slot;
} remixapi_MaterialInfoScreenCameraEXT;

typedef struct remixapi_DenoisingCapabilitiesEXT {
  uint32_t structSize, reblurShSupported, taaSupported, reserved;
  char unavailableReason[256];
} remixapi_DenoisingCapabilitiesEXT;
// Denoiser values: 0 = off, 1 = REBLUR_DIFFUSE_SH. Requested settings survive
// progressive accumulation; active settings describe the last submitted world.
// Timings describe the previous completed world frame. gpuTaaMilliseconds includes
// final resolve and guide-history copies, including copy-only work with TAA off.
// memoryBytes includes owned temporal textures and constant buffers, not tracing
// targets, acceleration structures, descriptors, pipelines or driver allocations.
typedef struct remixapi_DenoisingStatisticsEXT {
  uint32_t structSize, requestedDenoiser, activeDenoiser, requestedTaa;
  uint32_t activeTaa, validationEnabled, redrawRequested, gpuTimingsAvailable;
  uint64_t historyResets, renderedFrames, historyFrames, memoryBytes, peakMemoryBytes;
  float gpuConfidenceMilliseconds, gpuDenoiseMilliseconds, gpuTaaMilliseconds, reserved;
  char unavailableReason[256];
} remixapi_DenoisingStatisticsEXT;

#ifdef __cplusplus
extern "C" {
#endif

// Host passes: screen-space passes with the host's own shaders, replayed
// after the frame is composed and before its UI: the host's post-processing
// effects (a Source game's pp/* materials, screen overlays), on its own
// render targets or the frame. The host compiles its D3D9 shaders to SPIR-V
// (DXVK's dxso layout: descriptor set 0 with the bindings it numbers, a push
// constant block at offset 0 for both stages, vertex locations by semantic)
// and registers them once; each frame it submits the passes in order, with
// their constants, samplers, vertices and target. The runtime copies all the
// data at submit; nothing need outlive the call.
typedef uint64_t remixapi_HostShaderHandleEXT; // 0: none
// A host render target, by the host's own identity of it (a texture); 0 is
// the frame itself, as composed so far (raw bytes, UNORM). Handles from
// REMIXAPI_HOST_TARGET_RESERVED_EXT up are the runtime's own images: passes
// sample them, nothing targets, copies into or updates them.
typedef uint64_t remixapi_HostTargetHandleEXT;
#define REMIXAPI_HOST_TARGET_RESERVED_EXT ((remixapi_HostTargetHandleEXT)0xffffffff00000000ull)
// The view depth of the path-traced frame: R32 float, the distance along the
// camera's axis (scene units) of the primary surface each pixel shows, 1e10
// and above where there is none; at the tracer's render resolution. A pass
// sampling it is skipped when no frame was path-traced.
#define REMIXAPI_HOST_TARGET_VIEW_DEPTH_EXT ((remixapi_HostTargetHandleEXT)0xffffffff00000001ull)
// The frame's depth buffer (D32 float, the frame's size), for the host to
// fill: a draw targeting it writes depth only (gl_FragDepth), a clear sets it
// to clearColor[0]. Draws on the frame test and write it as their `depth` says.
#define REMIXAPI_HOST_TARGET_FRAME_DEPTH_EXT ((remixapi_HostTargetHandleEXT)0xffffffff00000002ull)
#define REMIXAPI_HOST_DEPTH_WRITE_EXT 0x100u

#define REMIXAPI_HOST_SHADER_VERTEX_EXT 1u
#define REMIXAPI_HOST_SHADER_FRAGMENT_EXT 2u
// A shader's descriptor bindings (set 0). D3D9 shaders (DXVK's dxso) combine
// each texture with its sampler state: SAMPLER. D3D11 shaders (DXVK's dxbc)
// bind textures (shader resource views) and sampler states apart:
// SAMPLED_IMAGE and SAMPLER_STATE. A pass's remixapi_HostPassSamplerEXT
// entries serve all three by binding number: a SAMPLER binding takes the
// entry's target and filtering, a SAMPLED_IMAGE binding its target only, a
// SAMPLER_STATE binding its filtering only (linear, addressU, addressV).
#define REMIXAPI_HOST_BINDING_UNIFORM_BUFFER_EXT 0u
#define REMIXAPI_HOST_BINDING_SAMPLER_EXT 1u
#define REMIXAPI_HOST_BINDING_SAMPLED_IMAGE_EXT 2u
#define REMIXAPI_HOST_BINDING_SAMPLER_STATE_EXT 3u
typedef struct remixapi_HostShaderBindingEXT {
  uint32_t binding; // descriptor set 0, 0..4095
  uint32_t type;    // REMIXAPI_HOST_BINDING_*
} remixapi_HostShaderBindingEXT;

#define REMIXAPI_STRUCT_TYPE_HOST_SHADER_INFO_EXT ((remixapi_StructType)0x7fff1030)
typedef struct remixapi_HostShaderInfoEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t stage; // REMIXAPI_HOST_SHADER_*
  const uint32_t *code;
  uint64_t codeBytes;
  const remixapi_HostShaderBindingEXT *bindings;
  uint32_t bindingCount;
  uint32_t pushConstantBytes; // a block at offset 0, both stages (0: none)
} remixapi_HostShaderInfoEXT;

typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_RegisterHostShaderEXT)(const remixapi_HostShaderInfoEXT *info,
                                                                             remixapi_HostShaderHandleEXT *out);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_DestroyHostShaderEXT)(remixapi_HostShaderHandleEXT shader);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterHostShaderEXT(const remixapi_HostShaderInfoEXT *info,
                                                                         remixapi_HostShaderHandleEXT *out);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyHostShaderEXT(remixapi_HostShaderHandleEXT shader);

typedef struct remixapi_HostPassBufferEXT { // a uniform buffer binding's contents
  uint32_t binding;
  const void *data;
  uint32_t bytes;
} remixapi_HostPassBufferEXT;
typedef struct remixapi_HostPassSamplerEXT { // a combined image sampler binding
  uint32_t binding;
  remixapi_HostTargetHandleEXT target; // the host target sampled, 0 the frame
  uint32_t linear;                     // filter: 1 linear, 0 nearest
  uint32_t addressU, addressV;         // 0 repeat, 1 clamp to edge, 2 mirror
} remixapi_HostPassSamplerEXT;
typedef struct remixapi_HostPassVertexBindingEXT {
  const void *data;
  uint32_t bytes;
  uint32_t stride;
} remixapi_HostPassVertexBindingEXT;
typedef struct remixapi_HostPassAttributeEXT {
  uint32_t location;
  uint32_t binding; // a vertex binding of the pass
  uint32_t format;  // VkFormat
  uint32_t offset;
} remixapi_HostPassAttributeEXT;

#define REMIXAPI_HOST_PASS_DRAW_EXT 0u
#define REMIXAPI_HOST_PASS_COPY_EXT 1u  // source rectangle to target rectangle, scaled (linear or nearest)
#define REMIXAPI_HOST_PASS_CLEAR_EXT 2u // the target to clearColor
// How a host target's channels read when sampled, for formats Vulkan does
// not render to as such: luminance (red replicated, alpha one), luminance
// and alpha (red replicated, alpha kept), or no alpha (alpha reads one).
#define REMIXAPI_HOST_CHANNELS_DEFAULT_EXT 0u
#define REMIXAPI_HOST_CHANNELS_LUMINANCE_EXT 1u
#define REMIXAPI_HOST_CHANNELS_LUMINANCE_ALPHA_EXT 2u
#define REMIXAPI_HOST_CHANNELS_NO_ALPHA_EXT 3u
typedef struct remixapi_HostPassEXT {
  uint32_t kind; // REMIXAPI_HOST_PASS_*
  uint32_t
      depth; // a draw on the frame or its depth: 0 no depth test, else VkCompareOp + 1, | REMIXAPI_HOST_DEPTH_WRITE_EXT
  remixapi_HostTargetHandleEXT target;
  uint32_t targetWidth, targetHeight; // of a host target, for its creation
  uint32_t targetFormat;              // VkFormat of a host target (0: the frame's)
  uint32_t targetChannels;            // REMIXAPI_HOST_CHANNELS_*, likewise
  // A draw.
  remixapi_HostShaderHandleEXT vertexShader, fragmentShader;
  const remixapi_HostPassBufferEXT *buffers;
  uint32_t bufferCount;
  const remixapi_HostPassSamplerEXT *samplers;
  uint32_t samplerCount;
  const void *pushConstants;
  uint32_t pushConstantBytes;
  const remixapi_HostPassVertexBindingEXT *vertexBindings;
  uint32_t vertexBindingCount; // up to 4
  const remixapi_HostPassAttributeEXT *attributes;
  uint32_t attributeCount; // up to 16
  const uint32_t *indices;
  uint32_t indexCount;
  uint32_t topology; // 0 triangle list, 1 triangle strip
  uint32_t blendEnable;
  uint32_t srcColorBlend, dstColorBlend, colorBlendOp; // VkBlendFactor / VkBlendOp
  uint32_t srcAlphaBlend, dstAlphaBlend, alphaBlendOp;
  uint32_t colorWriteMask; // VkColorComponentFlags
  int32_t viewport[4];     // x, y, width, height in D3D's convention (y down); width 0: the whole target
  int32_t scissor[4];      // likewise
  // A copy.
  remixapi_HostTargetHandleEXT source;  // 0 the frame
  int32_t sourceRect[4], targetRect[4]; // x, y, width, height; width 0: whole
  uint32_t linear;
  // A clear.
  float clearColor[4];
} remixapi_HostPassEXT;

#define REMIXAPI_STRUCT_TYPE_HOST_PASS_LIST_EXT ((remixapi_StructType)0x7fff1031)
typedef struct remixapi_HostPassListEXT {
  remixapi_StructType sType;
  const void *pNext;
  const remixapi_HostPassEXT *passes;
  uint32_t passCount;
} remixapi_HostPassListEXT;
// Optional HostPassListEXT pNext: flags of the list's passes, flags[i] for
// passes[i] (passes past flagCount have none). REMIXAPI_HOST_PASS_ALWAYS_EXT:
// the pass is replayed whatever the user's host-pass setting (see
// GetHostPassesEnabledEXT): a host's draw that belongs on the frame (a game's
// own overlay the path tracer has no stand-in for) rather than an
// experimental replacement of the runtime's work. While the setting is off,
// a submission keeps only these passes (the rest are dropped, as without the
// flag) and target updates are kept for them.
#define REMIXAPI_HOST_PASS_ALWAYS_EXT 1u
#define REMIXAPI_STRUCT_TYPE_HOST_PASS_LIST_FLAGS_EXT ((remixapi_StructType)0x7fff1032)
typedef struct remixapi_HostPassListFlagsEXT {
  remixapi_StructType sType;
  const void *pNext;
  const uint32_t *flags; // REMIXAPI_HOST_PASS_* flags
  uint32_t flagCount;
} remixapi_HostPassListFlagsEXT;
// The passes of the next presented frame, in order (replacing any submitted
// before it this frame). Pipelines compile on first use in the background: a
// pass whose pipeline is not ready yet is skipped that frame.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SubmitHostPassesEXT)(const remixapi_HostPassListEXT *list);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SubmitHostPassesEXT(const remixapi_HostPassListEXT *list);

// A host target's contents from the host's memory: a texture an effect
// samples (RGBA or BGRA, 8 bits a channel, tightly packed), uploaded before
// the next frame's passes. The target takes the size and format given.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_UpdateHostTargetEXT)(remixapi_HostTargetHandleEXT target,
                                                                           uint32_t width, uint32_t height,
                                                                           uint32_t vkFormat, const void *pixels,
                                                                           uint64_t bytes);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateHostTargetEXT(remixapi_HostTargetHandleEXT target,
                                                                       uint32_t width, uint32_t height,
                                                                       uint32_t vkFormat, const void *pixels,
                                                                       uint64_t bytes);

// A volume (3D) texture's contents, likewise: sampled only (a 3D sampler
// in the shader), never a pass's target.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_UpdateHostVolumeEXT)(remixapi_HostTargetHandleEXT target,
                                                                           uint32_t width, uint32_t height,
                                                                           uint32_t depth, uint32_t vkFormat,
                                                                           const void *pixels, uint64_t bytes);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateHostVolumeEXT(remixapi_HostTargetHandleEXT target,
                                                                       uint32_t width, uint32_t height, uint32_t depth,
                                                                       uint32_t vkFormat, const void *pixels,
                                                                       uint64_t bytes);

// Whether host passes run: the user's setting (rtx.native.hostPasses, shown
// in the Rendering tab as "Pass-through game shaders (experimental)" to a host
// that has asked this). While it is off, SubmitHostPassesEXT succeeds but
// drops every pass not flagged REMIXAPI_HOST_PASS_ALWAYS_EXT, and target
// updates are kept only for those; a host polls this each frame and keeps
// its own work (recording, compiling shaders) for when it is on.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetHostPassesEnabledEXT)(remixapi_Bool *enabled);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetHostPassesEnabledEXT(remixapi_Bool *enabled);

typedef struct remixapi_HostPassStatisticsEXT {
  uint64_t structSize;
  uint32_t passes, draws, copies, clears; // last frame
  uint32_t skipped;                       // passes skipped last frame (pipeline compiling, invalid)
  uint32_t shaders, targets;              // registered / alive
  uint32_t pipelines;                     // created so far
  // Why passes were skipped, last frame: copy source missing or rectangles
  // out of bounds, shaders missing, pipeline not ready, descriptors
  // exhausted, a buffer binding without data, a sampler binding without a
  // sampler, a sampled target without an image, no view depth this frame.
  uint32_t skippedCopySource, skippedShaders, skippedPipeline, skippedDescriptors;
  uint32_t skippedBuffer, skippedSampler, skippedTarget, skippedViewDepth;
  // Versioned by structSize (a caller's smaller struct, without these, gets
  // the fields it has). Last submission: passes flagged
  // REMIXAPI_HOST_PASS_ALWAYS_EXT (counted in `passes` too when replayed),
  // and passes dropped because the setting is off.
  uint32_t always, dropped;
} remixapi_HostPassStatisticsEXT;
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetHostPassStatisticsEXT)(remixapi_HostPassStatisticsEXT *out);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetHostPassStatisticsEXT(remixapi_HostPassStatisticsEXT *out);

// A host's default for a runtime setting (a SetConfigVariable key): it applies
// to every profile that has no value of its own for the key (the key is not
// in its saved file; an unsaved or new profile has none), now and when a
// profile is loaded or created later. What a user saved stays. Settings a
// host wants regardless go through SetConfigVariable.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetConfigDefaultEXT)(const char *key, const char *value);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetConfigDefaultEXT(const char *key, const char *value);

typedef enum remixapi_InputEventType {
  REMIXAPI_INPUT_RESET = 1, // Release all held input (focus/capture change or queue overflow).
  REMIXAPI_INPUT_KEY = 2,
  REMIXAPI_INPUT_TEXT = 3, // One Unicode scalar, independent of physical key events.
  REMIXAPI_INPUT_MOUSE_BUTTON = 4,
  REMIXAPI_INPUT_MOUSE_MOTION = 5,
  REMIXAPI_INPUT_MOUSE_WHEEL = 6,
} remixapi_InputEventType;

typedef struct remixapi_InputEvent {
  uint32_t type;
  uint32_t code; // KEY: remixapi_Key; TEXT: Unicode; BUTTON: remixapi_MouseButtonBits.
  uint32_t down;
  uint32_t repeat;
  float x, y;   // Logical window position; wheel events contain wheel displacement.
  float dx, dy; // Relative logical-unit motion, for MOUSE_MOTION only.
} remixapi_InputEvent;

typedef struct remixapi_WindowMetrics {
  uint32_t logicalWidth, logicalHeight;
  uint32_t drawableWidth, drawableHeight;
} remixapi_WindowMetrics;

// All calls use the startup thread. Poll drains up to capacity events in order.
// capacity == 0 queries the queued count without consuming it. Overflow emits RESET.
// Events include ImGui-captured input, allowing application UI shortcuts. Clients
// must honor WindowState.guiWantsKeyboard/guiWantsMouse for gameplay dispatch.
// Existing state polling APIs remain independent and exclude captured input.
// The panel's shortcut is the runtime's (Alt+X unless the user sets another in
// the panel's header): remixapi_PumpEvents opens and closes the panel on it,
// and its key events and text reach neither ImGui nor this queue. Clients need
// no key of their own; GetUIState reports the panel.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_PollInputEvents)(remixapi_InputEvent *events, uint32_t capacity,
                                                                       uint32_t *out_count);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetTextInputEnabled)(remixapi_Bool enabled);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetWindowMetrics)(remixapi_WindowMetrics *out_metrics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PollInputEvents(remixapi_InputEvent *events, uint32_t capacity,
                                                                   uint32_t *out_count);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetTextInputEnabled(remixapi_Bool enabled);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetWindowMetrics(remixapi_WindowMetrics *out_metrics);
// Successful/suboptimal Vulkan queue presentations since Startup. Skipped frames do not count.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetPresentationCount)(uint64_t *out_count);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetPresentationCount(uint64_t *out_count);

typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetBackendCapabilitiesEXT)(
    remixapi_BackendCapabilitiesEXT *capabilities);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetPreparationStatisticsEXT)(
    remixapi_PreparationStatisticsEXT *statistics);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetLocalLightStatisticsEXT)(
    remixapi_LocalLightStatisticsEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetLocalLightStatisticsEXT(remixapi_LocalLightStatisticsEXT *statistics);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetLocalLightSetStatisticsEXT)(
    remixapi_LocalLightSetStatisticsEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetLocalLightSetStatisticsEXT(remixapi_LocalLightSetStatisticsEXT *statistics);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetLocalLightGuidingStatisticsEXT)(
    remixapi_LocalLightGuidingStatisticsEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetLocalLightGuidingStatisticsEXT(remixapi_LocalLightGuidingStatisticsEXT *statistics);
// A potentially-visible-set partition of the host's world (a BSP with
// cluster-to-cluster visibility, as in id Tech and Source maps), in world
// space. Local lights in clusters not potentially visible from a shading
// point's cluster are excluded from light selection; lights without a
// cluster, lights that move, new lights until they have been at rest for a
// while, and the emitters of REMIXAPI_INSTANCE_DYNAMIC_EXT instances remain
// visible from everywhere (remixapi_LocalLightSetStatisticsEXT). Points in leaves
// without a cluster are unrestricted. nullptr (or nodeCount 0) clears it.
typedef struct remixapi_VisibilityPartitionEXT {
  uint32_t structSize;
  uint32_t nodeCount;
  const float *planes;     // nodeCount * 4: normal xyz, distance; front when dot(n, p) - d >= 0
  const int32_t *children; // nodeCount * 2: front, back; >= 0 node, < 0 leaf (-1 - leaf index)
  uint32_t leafCount;
  const int32_t *leafClusters; // leafCount; -1: no cluster
  uint32_t clusterCount;
  uint32_t rowBytes;         // >= (clusterCount + 7) / 8
  const uint8_t *visibility; // clusterCount rows; bit c of row s: cluster c potentially visible from s
} remixapi_VisibilityPartitionEXT;
// Rewrites a region of a texture registered with RegisterUITexture in place:
// rows of width * 4 bytes, rowPitch apart, in R8G8B8A8 or B8G8R8A8 (the
// registration's format family need not match). Only the region is uploaded
// to the GPU, so hosts can forward partial updates of large UI surfaces
// (e.g. an embedded browser's dirty rectangles) without re-registering them.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_UpdateUITextureRegionEXT)(remixapi_UITextureHandle id, uint32_t x,
                                                                                uint32_t y, uint32_t width,
                                                                                uint32_t height, remixapi_Format format,
                                                                                const void *pixels, uint32_t rowPitch);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateUITextureRegionEXT(remixapi_UITextureHandle id, uint32_t x,
                                                                            uint32_t y, uint32_t width, uint32_t height,
                                                                            remixapi_Format format, const void *pixels,
                                                                            uint32_t rowPitch);

// Rewrites a region of a dynamic texture (remixapi_TextureInfoDynamicEXT) in
// place: rows of width * 4 bytes, rowPitch apart, in R8G8B8A8 or B8G8R8A8 (the
// byte order may differ from the creation's; the texture keeps the colour
// space it was created with). The region must lie inside the texture.
// The pixels are copied before the call returns. Rows equal to the texture's
// current ones are skipped: an update that changes nothing costs a comparison
// and counts as no update (no upload, no scene change). Otherwise the smallest
// rectangle that holds the changed pixels is uploaded once, by the first frame
// recorded after the call, ahead of everything that frame draws (traced,
// rasterized, UI), into the same image; several updates before that frame
// make one upload of their union. Frames already recorded keep the pixels
// they were recorded with: on the GPU the copy waits for their reads. With
// asynchronous presentation ("rtx.native.asyncPresent") a frame that was
// submitted but not yet recorded when the call was made shows the new pixels
// (they are at most one Present early). INVALID_ARGUMENTS: an unknown handle,
// a texture that was not created dynamic, a region outside it, another
// format, rowPitch < width * 4.
// Optional named export: resolve "remixapi_UpdateTextureEXT"; a runtime
// without it ignores remixapi_TextureInfoDynamicEXT as well.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_UpdateTextureEXT)(remixapi_TextureHandle handle, uint32_t x,
                                                                        uint32_t y, uint32_t width, uint32_t height,
                                                                        remixapi_Format format, const void *pixels,
                                                                        uint32_t rowPitch);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateTextureEXT(remixapi_TextureHandle handle, uint32_t x,
                                                                    uint32_t y, uint32_t width, uint32_t height,
                                                                    remixapi_Format format, const void *pixels,
                                                                    uint32_t rowPitch);

// Texture and material turnover, cumulative since Startup (a host reports the
// difference between two calls, e.g. per frame). Creating a texture or a
// material costs GPU objects on the first frame that uses it and their
// destruction a few frames after its last one: a host that makes new ones
// every frame pays both every frame (gpuCreateMilliseconds within the frame's
// recording, gpuDestroyMilliseconds within Present's setup).
typedef struct remixapi_TextureStatisticsEXT {
  uint32_t structSize, reserved;
  // CreateTexture calls that stored a new image (not an identical
  // resubmission), DestroyTexture calls that removed one; likewise materials.
  uint64_t texturesCreated, texturesDestroyed;
  uint64_t materialsCreated, materialsDestroyed;
  // remixapi_UpdateTextureEXT: calls, those that changed pixels, the bytes
  // they copied, and the CPU time of all calls (comparison and copy).
  uint64_t updateCalls, updates, updateBytes;
  double updateMilliseconds;
  // Changed regions staged to the GPU (one per updated texture and frame that
  // uses it), their bytes, and the CPU time of staging them.
  uint64_t uploads, uploadBytes;
  double uploadMilliseconds;
  // GPU objects made for new textures (image, memory, view, staged pixels)
  // and new materials (sampler, descriptor set), and those released.
  uint64_t gpuTexturesCreated, gpuTexturesDestroyed;
  uint64_t gpuMaterialsCreated, gpuMaterialsDestroyed;
  uint64_t gpuCreateBytes; // pixels staged for new textures
  double gpuCreateMilliseconds, gpuDestroyMilliseconds;
  // Emissive light estimates made again because a dynamic texture changed
  // (surfaces), and their CPU time.
  uint64_t emissionRefreshes;
  double emissionRefreshMilliseconds;
} remixapi_TextureStatisticsEXT;
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetTextureStatisticsEXT)(
    remixapi_TextureStatisticsEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetTextureStatisticsEXT(remixapi_TextureStatisticsEXT *statistics);

// The last presented frame (the game's image, without OpenRemix's own UI) as
// 8-bit BGRA with alpha 255, sRGB-encoded SDR: HDR10 output tone mapped as an
// SDR screenshot is. Fills width x height pixels, rowPitch bytes apart,
// scaled (nearest) from the presented size, which presentedWidth and
// presentedHeight report. Waits for the GPU: for occasional readbacks, such as
// a D3D9 host's screenshot of its own back buffer, which OpenRemix never draws.
// FEATURE_UNAVAILABLE: nothing presented yet (or the swapchain cannot be
// copied from).
typedef struct remixapi_PresentedImageEXT {
  uint32_t width, height, rowPitch;
  void *pixels;                             // rowPitch * height bytes
  uint32_t presentedWidth, presentedHeight; // out
} remixapi_PresentedImageEXT;
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_ReadPresentedImageEXT)(remixapi_PresentedImageEXT *image);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ReadPresentedImageEXT(remixapi_PresentedImageEXT *image);

typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetVisibilityPartitionEXT)(
    const remixapi_VisibilityPartitionEXT *partition);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_SetVisibilityPartitionEXT(const remixapi_VisibilityPartitionEXT *partition);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetCameraCullingStatisticsEXT)(
    remixapi_CameraCullingStatisticsEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetCameraCullingStatisticsEXT(remixapi_CameraCullingStatisticsEXT *statistics);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetShadowStatisticsEXT)(remixapi_ShadowStatisticsEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetShadowStatisticsEXT(remixapi_ShadowStatisticsEXT *statistics);

typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetSharcStatisticsEXT)(remixapi_SharcStatisticsEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetSharcStatisticsEXT(remixapi_SharcStatisticsEXT *statistics);

typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetTracingStatisticsEXT)(
    remixapi_TracingStatisticsEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetBackendCapabilitiesEXT(remixapi_BackendCapabilitiesEXT *capabilities);
// remixapi_InstanceInfoBackdropEXT is implemented: returns SUCCESS.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_InstanceBackdropSupportedEXT)(void);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_InstanceBackdropSupportedEXT(void);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetPreparationStatisticsEXT(remixapi_PreparationStatisticsEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetTracingStatisticsEXT(remixapi_TracingStatisticsEXT *statistics);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetFramePerformanceEXT)(remixapi_FramePerformanceEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetFramePerformanceEXT(remixapi_FramePerformanceEXT *statistics);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetFrameBreakdownEXT)(remixapi_FrameBreakdownEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetFrameBreakdownEXT(remixapi_FrameBreakdownEXT *statistics);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetGpuPassTimingsEXT)(remixapi_GpuPassTimingsEXT *timings);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetGpuPassTimingsEXT(remixapi_GpuPassTimingsEXT *timings);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetDenoisingCapabilitiesEXT)(
    remixapi_DenoisingCapabilitiesEXT *capabilities);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_GetDenoisingStatisticsEXT)(
    remixapi_DenoisingStatisticsEXT *statistics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetDenoisingCapabilitiesEXT(remixapi_DenoisingCapabilitiesEXT *capabilities);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_GetDenoisingStatisticsEXT(remixapi_DenoisingStatisticsEXT *statistics);

// Optional MeshInfo pNext: the mesh already registered under this hash,
// deformed. The new registration replaces its vertex data and stays the same
// object: with the same surfaces, vertex counts and index values the tracer
// refits the acceleration structure it has (bone-skinned meshes included,
// which MeshInfoUpdateEXT leaves to the per-instance skin path) and motion
// vectors follow each vertex from its previous position. Draws already queued
// keep the previous vertices. With another topology the object starts over
// (one full build); with another surface count, or nothing registered under
// the hash, the struct is ignored. Honoured while
// "rtx.native.meshes.refreshInPlace" is on; otherwise a repeated hash is the
// plain re-registration it always was. A host morphing a face every frame
// uses this instead of a new hash a frame, which would be a new object
// without history each time.
#define REMIXAPI_STRUCT_TYPE_MESH_DEFORM_EXT ((remixapi_StructType)0x7fff1018)
typedef struct remixapi_MeshInfoDeformEXT {
  remixapi_StructType sType;
  const void *pNext;
} remixapi_MeshInfoDeformEXT;

// A deformation (as MeshInfoDeformEXT) that sends only what changed: new
// positions, and optionally normals and texture coordinates, for surfaces of
// the mesh registered under `mesh`. Indices, colours, skinning, sprite data and
// materials stay as registered, so the mesh keeps its topology and, while
// "rtx.native.meshes.refreshInPlace" is on, its identity: the tracer refits
// its acceleration structure and motion vectors follow each vertex. (With the
// setting off the result is a new registration under the same hash, as
// CreateMesh would make.) Draws already queued keep the previous vertices.
// Arrays are tightly packed floats, vertexCount long; vertexCount must equal
// the surface's. Normals are normalized as CreateMesh does (a zero or
// non-finite one leaves the vertex without a normal); positions and texture
// coordinates must be finite. INVALID_ARGUMENTS: an unknown mesh, a surface
// index out of range or given twice, another vertex count, a missing or
// non-finite position. Nothing is changed then. For hosts that re-skin or
// morph meshes on the CPU every frame (a D3D9 game's flexed faces).
typedef struct remixapi_MeshVerticesEXT {
  uint32_t surface;       // index into the mesh's surfaces
  uint32_t vertexCount;   // the surface's vertex count
  const float *positions; // vertexCount * 3
  const float *normals;   // vertexCount * 3, or null: unchanged
  const float *texcoords; // vertexCount * 2, or null: unchanged
} remixapi_MeshVerticesEXT;
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_UpdateMeshVerticesEXT)(remixapi_MeshHandle mesh,
                                                                             uint32_t surfaceCount,
                                                                             const remixapi_MeshVerticesEXT *surfaces);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateMeshVerticesEXT(remixapi_MeshHandle mesh,
                                                                         uint32_t surfaceCount,
                                                                         const remixapi_MeshVerticesEXT *surfaces);

// Optional InstanceInfo pNext (DrawInstance, AddSceneInstancesEXT,
// SetSceneInstanceEXT): every surface of the mesh is shaded with this material
// instead of the one its surface was created with, so one mesh serves any
// number of materials. A null handle, or one that is not a live material,
// keeps the mesh's own. Instances of one mesh share its geometry and
// acceleration structure across materials, except materials that alpha-test
// or blend, which keep a copy each (their opacity is part of the structure).
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_EXT ((remixapi_StructType)0x7fff1019)
typedef struct remixapi_InstanceInfoMaterialEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_MaterialHandle material;
} remixapi_InstanceInfoMaterialEXT;

// Optional InstanceInfo pNext (DrawInstance, AddSceneInstancesEXT,
// SetSceneInstanceEXT): the instance's texture coordinates become
// uv x scale + offset + velocity x t, t the runtime's clock in seconds, before
// a sprite sheet picks its cell. A scrolling texture (water, lava, a
// conveyor) moves without new vertices or materials: a constant velocity
// scrolls it with no further calls, and a host that drives the offset itself
// replaces the description (SetSceneInstanceEXT) when it changes. Every map
// of the material follows. With an offset or a velocity, which a host may
// change every frame, its emitters are estimated where the texture rests
// (scaled, not moved) and alpha-tested texels are tested by every ray. An
// instance with a velocity is drawn as an animated material's are, outside
// the persistent static set. Absent: scale 1, offset and velocity 0.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_TEXCOORD_TRANSFORM_EXT ((remixapi_StructType)0x7fff1028)
typedef struct remixapi_InstanceInfoTexcoordTransformEXT {
  remixapi_StructType sType;
  const void *pNext;
  float scale[2], offset[2], velocity[2];
} remixapi_InstanceInfoTexcoordTransformEXT;

// Scene instances: instances the runtime keeps. A host describes a placement
// once (AddSceneInstancesEXT, any number in one call) and every frame's
// DrawSceneInstancesEXT draws all that are visible, in place of one
// DrawInstance per placement per frame; what is static among them is also kept
// in the tracer's tables from frame to frame. Handles are nonzero. An
// instance's description is an InstanceInfo with the pNext chain DrawInstance
// takes; it is copied. Remove an instance before destroying its mesh.
// Named exports; resolve with GetProcAddress/dlsym.
#define REMIXAPI_SCENE_INSTANCE_TRANSFORM_EXT 0x1u
#define REMIXAPI_SCENE_INSTANCE_VISIBLE_EXT 0x2u
typedef struct remixapi_SceneInstanceChangeEXT {
  uint64_t instance;
  uint32_t fields; // REMIXAPI_SCENE_INSTANCE_*: which of the members below apply
  remixapi_Bool visible;
  remixapi_Transform transform; // the instance's new transform, not a delta
} remixapi_SceneInstanceChangeEXT;
// outInstances: count handles, in the order of infos. Nothing is added when a
// description is refused.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_AddSceneInstancesEXT)(const remixapi_InstanceInfo *infos,
                                                                            uint32_t count, uint64_t *outInstances);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_AddSceneInstancesEXT(const remixapi_InstanceInfo *infos,
                                                                        uint32_t count, uint64_t *outInstances);
// Replaces an instance's whole description; its visibility stays.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_SetSceneInstanceEXT)(uint64_t instance,
                                                                           const remixapi_InstanceInfo *info);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetSceneInstanceEXT(uint64_t instance,
                                                                       const remixapi_InstanceInfo *info);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_ChangeSceneInstancesEXT)(
    const remixapi_SceneInstanceChangeEXT *changes, uint32_t count);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_ChangeSceneInstancesEXT(const remixapi_SceneInstanceChangeEXT *changes, uint32_t count);
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_RemoveSceneInstancesEXT)(const uint64_t *instances,
                                                                               uint32_t count);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RemoveSceneInstancesEXT(const uint64_t *instances, uint32_t count);
// origin: three floats subtracted from every scene instance's translation
// this frame (a host that rebases its world around the camera gives the same
// origin it subtracts from its camera and lights); null for none.
typedef remixapi_ErrorCode(REMIXAPI_PTR *PFN_remixapi_DrawSceneInstancesEXT)(const float *origin);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawSceneInstancesEXT(const float *origin);

#ifdef __cplusplus
}
#endif

#define REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT ((remixapi_StructType)0x7fff1011)
typedef struct remixapi_MaterialNormalEncodingEXT {
  remixapi_StructType sType;
  const void *pNext;
  // 0 legacy auto XY/XYZ, 1 hemisphere-octahedral, 2 tangent DX, 3 tangent GL,
  // 4 Source engine self-shadowed bump ($ssbump: RGB weights of its three
  // bump basis directions), 5 Source engine water: a tangent DX map sampled
  // as the Water shader samples its $normalmap (remixapi_MaterialNormalScrollEXT)
  uint32_t encoding;
} remixapi_MaterialNormalEncodingEXT;

// Optional pNext on MaterialInfo, with normal encoding 5: the scroll rates (UV
// units per second, of the runtime's clock) of Source's Water shader. The map
// is sampled at the surface's UVs moved by base x t (the scroll its
// $bumptransform gets from a TextureScroll proxy) and, with multi-texture
// layers (layers 3: $scroll1 set), averaged with samples at
// 0.1 x (u + v, v - u) + layer1 x t and 0.45 x (v, u) + layer2 x t.
// Progressive path-tracing accumulation evaluates these procedural scrolls at
// time zero so history describes a fixed surface. Ordinary rendering advances
// the runtime clock. Host texture/geometry updates still invalidate history.
// Encoding 5 shares the material's packed fields with
// remixapi_MaterialPbrRemapEXT: such a material has no roughness or metallic
// map.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_SCROLL_EXT ((remixapi_StructType)0x7fff101b)
typedef struct remixapi_MaterialNormalScrollEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t layers; // 1 or 3
  float base[2], layer1[2], layer2[2];
} remixapi_MaterialNormalScrollEXT;

// Optional MaterialInfo pNext for a measured finite dielectric sheet. Its
// transmission tint samples uv + rate * animationSeconds. A host can use the
// same rate as its normal texture, without encoding engine-specific semantics.
// Progressive rendering uses its fixed animation clock. Nonzero rates need a
// matching MaterialBakeTintAverageEXT policy before entering a frozen bake.
// Existing materials retain zero rates and their established cache identities.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_TRANSMITTANCE_SCROLL_EXT ((remixapi_StructType)0x7fff1070)
typedef struct remixapi_MaterialTransmittanceScrollEXT {
  remixapi_StructType sType;
  const void *pNext;
  float rate[2];
} remixapi_MaterialTransmittanceScrollEXT;

// Optional MaterialInfo pNext for a measured finite sheet. Static transport
// samples its transmission-tint UV uniformly on each requested axis, averaging
// optical interactions rather than pre-averaging colours or extinction. This
// explicitly selects a stationary distribution; it does not infer an animation
// period. Interactions sample independent phases, not a coherent global time.
// Other UV axes retain their spatial detail. Live rendering ignores the
// policy and uses the actual tint clock. Tint images must still be immutable.
// Every scrolling tint axis must be averaged before accepting a frozen bake.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_BAKE_TINT_AVERAGE_EXT ((remixapi_StructType)0x7fff1071)
#define REMIXAPI_MATERIAL_BAKE_TINT_U_EXT 0x1u
#define REMIXAPI_MATERIAL_BAKE_TINT_V_EXT 0x2u
typedef struct remixapi_MaterialBakeTintAverageEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t axes; // zero, U, V, or U|V
} remixapi_MaterialBakeTintAverageEXT;

// Optional pNext on opaque MaterialInfo: how the roughness and metallic maps'
// texels become values, so a game's own masks serve as PBR maps unconverted.
// Per map: x = texel[channel]; inverted, x = 1 - x; x = x^exponent; then
// curve 0: value = bias + scale * x,
// curve 1: a Blinn-Phong exponent n = bias + scale * x, value = the perceptual
//          roughness of the same lobe, (2 / (n + 2))^(1/4).
// Absent: the red channel as it is (channel 0, curve 0, bias 0, scale 1,
// exponent 1). exponent: 0 to 15.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_REMAP_EXT ((remixapi_StructType)0x7fff101a)
typedef struct remixapi_MaterialPbrChannelEXT {
  uint32_t channel; // 0 red, 1 green, 2 blue, 3 alpha
  remixapi_Bool invert;
  uint32_t curve;
  float exponent, bias, scale;
} remixapi_MaterialPbrChannelEXT;
typedef struct remixapi_MaterialPbrRemapEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_MaterialPbrChannelEXT roughness, metallic;
} remixapi_MaterialPbrRemapEXT;

// Optional pNext on opaque MaterialInfo: the colour of its metal (linear, 0
// to 1). Where the material is metallic it reflects this colour instead of
// its albedo, which its diffuse keeps: a game's dark base texture under a
// strong reflection (Source's envmapped metal, its $envmaptint) is chrome,
// not dark metal, and partly metal texels keep their own diffuse colour.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_METAL_COLOR_EXT ((remixapi_StructType)0x7fff1023)
typedef struct remixapi_MaterialMetalColorEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Float3D color;
} remixapi_MaterialMetalColorEXT;

// Optional pNext on opaque MaterialInfo: its roughness map is read no
// sharper than mip level roughnessMinimumLod (0 to 15). A mask painted with
// crisp patches (a game's reflection mask) then changes the reflection's
// sharpness over about 2^roughnessMinimumLod texels at their edges, while
// areas wider than that keep their values; seen from afar, where the map is
// read blurrier anyway, nothing changes. The metallic map, which decides
// where the albedo shows, stays as sharp as the view allows.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_BLUR_EXT ((remixapi_StructType)0x7fff1024)
typedef struct remixapi_MaterialPbrBlurEXT {
  remixapi_StructType sType;
  const void *pNext;
  float roughnessMinimumLod;
} remixapi_MaterialPbrBlurEXT;

// Optional pNext on opaque MaterialInfo: gradient-mapped albedo, as Source's
// Infected shader (Left 4 Dead 2's common infected) composites one texture
// set into many looks. The material's albedo texture B is then read as stored
// (no sRGB decode): B.a is luminance, B.b masks skin (above 1/2) from cloth
// (below), and the quarter-size sprite S = B(uv / 2 + spriteOffset) holds a
// grime mask in S.r and a blood mask in S.g. With G the gradient texture
// (sRGB-decoded, clamped, bilinear) and D the detail texture (as stored):
//   L = B.a * B.a
//   c = mix(L, G(B.a, skinRow).rgb, saturate(2 B.b - 1))
//   c = mix(c, G(B.a, clothRow).rgb, saturate(1 - 2 B.b))
//   c = mix(c, 2 L D(d).rgb, saturate(2 S.r - 1))      (without a detail
//       texture this step is skipped), d = (dot(detailU, uv), dot(detailV, uv))
//   s = smoothstep(bloodRange[0], bloodRange[1], saturate(2 S.g - 1) + W)
//   c = min(c * mix(1, bloodColor, s), 1)
//   albedo = mix(c, U(d.y, d.x).rgb, saturate(burnStrength * (2 -
//       saturate(2 B.b - 1)) + U(d.y, d.x).a - 1))   (with a burn texture U)
// W is the wound term of remixapi_InstanceInfoWoundCutEXT (0 without one). The
// textures are named as material textures are (a CreateTexture hash "0x..." or
// a file path; an empty detail or burn texture skips its step; the burn
// texture is read like an albedo texture, sRGB-decoded if it is sRGB-encoded,
// and repeats); a row selects the
// gradient's v coordinate (row + 0.5) / rows, rows its height in texels. The
// composite takes the albedo texel's place in the material's colour (the
// vertex colour and albedoConstant still modulate it). B.a holds luminance,
// not coverage: no alpha test or thin film, and opacityConstant alone gives
// the material's opacity (below 1 it is blended like any opaque material with
// that opacity: a model fading out). bloodColor: 0 to 1; bloodRange[0] below
// bloodRange[1]; burnStrength: 0 to 1; the rows, spriteOffset, detailU,
// detailV, bloodRange and burnStrength are kept as half floats (magnitudes up
// to 65504).
#define REMIXAPI_STRUCT_TYPE_MATERIAL_GRADIENT_MAP_EXT ((remixapi_StructType)0x7fff1027)
typedef struct remixapi_MaterialGradientMapEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Path gradientTexture;
  remixapi_Path detailTexture;
  float skinRow;
  float clothRow;
  float spriteOffset[2];
  float detailU[2];
  float detailV[2];
  remixapi_Float3D bloodColor;
  float bloodRange[2];
  remixapi_Path burnTexture;
  float burnStrength;
} remixapi_MaterialGradientMapEXT;

// One wound ellipsoid of remixapi_InstanceInfoWoundCutEXT: a point p maps to
// e = (dot(axisX, p - center), dot(axisY, p - center), dot(axisZ, p - center)),
// the unit ellipsoid in e being the wound. The axes carry the ellipsoid's
// inverse scale.
typedef struct remixapi_WoundEllipsoidEXT {
  remixapi_Float3D center;
  remixapi_Float3D axisX;
  remixapi_Float3D axisY;
  remixapi_Float3D axisZ;
} remixapi_WoundEllipsoidEXT;

// Optional pNext on InstanceInfo: wounds cut out of the instance's surfaces,
// as Source's Infected shader does. Each vertex is evaluated at its position
// as submitted in the mesh (the bind pose: before the instance's bone
// transforms), with e1, e2 of ellipsoids[0], [1] and k = secondBlend:
//   el = mix(e1, e2, k)
//   q(e) = (e.x, e.y) / (sqrt(1 - e.z^2) * mappingScale)  (|e.z| >= 1: none)
//   w = (q(e1), q(el)) * projectionScale + projectionBias   (4 components)
//   h = length(e2) + k
// At a surface point w and h are interpolated across the triangle like
// texture coordinates. With T the cut texture's red channel (as stored, the
// border black), each half of w whose q exists at all three vertices gives
//   W = max(T(saturate(w.xy) * cutScaleBias[0].xy + cutScaleBias[0].zw),
//           T(saturate(w.zw) * cutScaleBias[1].xy + cutScaleBias[1].zw))
// (a half without q gives 0). The point is cut away, for every ray, where
// W > threshold or h < 1. W also deepens remixapi_MaterialGradientMapEXT's
// blood. An instance without a skinned or plain mesh position ignores this.
// The cut texture is named as material textures are and read bilinearly at
// its finest mip, black outside 0..1; mappingScale above 0, every value
// finite. A wounded instance is traced as a dynamic one (never part of the
// persistent static set); halo, mask source and overlay, and wireframe
// instances ignore this.
#define REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_WOUND_CUT_EXT ((remixapi_StructType)0x7fff1026)
typedef struct remixapi_InstanceInfoWoundCutEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Path cutTexture;
  remixapi_WoundEllipsoidEXT ellipsoids[2];
  float mappingScale;
  float secondBlend;
  float projectionScale[4];
  float projectionBias[4];
  float cutScaleBias[2][4];
  float threshold;
} remixapi_InstanceInfoWoundCutEXT;

// Optional pNext on an opaque MaterialInfo: a portal's rim, as Portal 2's
// PortalRefract shader draws it ($Stage 2: the glow around an opening and,
// while the portal has no partner, the swirl that fills it). The effect takes
// the place of the material's colour and emission wherever it is shaded: the
// surface reflects nothing, covers with alpha a and emits a x C x the
// emissive colour and intensity; the albedo texture N is its noise (read as
// stored, with the material's sampler) and colorTexture R its colour ramp
// (read like an albedo texture: sRGB-decoded if sRGB-encoded; clamped). Per
// vertex, the colour's red is the opening o (0 closed to 1 open) and its
// green the static s (1: no partner shown), as stored (no sRGB decode),
// interpolated like any vertex colour (a mesh keeps them equal at its
// vertices). With the mesh's texture coordinate uv (0..1 over the engine's
// quad, v down; the instance's texture coordinate transform is not applied)
// and t the runtime's clock in seconds:
//   c = 1.075 uv - 0.0375, d = |2c - 1|, k = smoothstep(o), r = k^2
//   e = d <= r ? saturate((d - r + 0.3) / 0.3) : 1 - saturate(13.333 (d - r))
//   E = e max(saturate(2.5 k), s)
//   w = 0.3 ((uv - 0.5) / saturate(o + 0.001) + 0.5)
//   A = w + (0.0275 t, 0), B = w - (0.0275 t, 0)
//   n1 = N(A), n2 = N(B.yx - 0.02 n1.xy), n3 = N(A - 0.02 n2.xy)
//   h = (n2.g + n3.g) / 2
//   P = 1 - smoothstep(saturate((h - E + 0.875) / 1.75))
//   Q = 1 - smoothstep(saturate((1 - s) / h))
//   I = saturate(E P + (d <= r ? Q : 0))
//   x = 1.5 I sqrt(P) (0.8 |c.y|^1.5 + 0.2)
//   C = R(x, 0.5).rgb colorScale, a = saturate(1.5 I)
// (smoothstep(x) = x^2 (3 - 2x)). A blended instance (source alpha over) shows
// it as the game's overlay, rasterized or traced (a sheet of coverage a
// emitting C). Traced rays meet an instance that is not blended wherever its
// triangles are, its coverage untested (a host cuts its mesh to where a is
// high: the alpha test reads the albedo texture as usual), and it emits a x C
// there; light and shadow rays take any surface of it by its albedo texture.
// Its emitters are estimated with h at 0.42, 0.53 and 0.64 (around the mean
// of Portal 2's noise). The raster backend evaluates the same (the Metal
// backend draws the plain material). Not with a gradient map, a thin film, a
// portal or a translucent material; colorTexture is named as material
// textures are and required; colorScale finite, at least 0.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_PORTAL_RIM_EXT ((remixapi_StructType)0x7fff1029)
typedef struct remixapi_MaterialPortalRimEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Path colorTexture;
  float colorScale;
} remixapi_MaterialPortalRimEXT;

// Optional pNext on an opaque MaterialInfo: Portal 2's SolidEnergy surface
// (a light bridge, effects/projected_wall: $detail1blendmode 1, $detail2blendmode
// 0, $additive), a glowing sheet seen through. Its base B is the material's
// albedo texture, D1 detail1Texture and D2 detail2Texture (sRGB as material
// textures are), at mesh UVs (the surface's UV transform left out) moved by
// the runtime's clock t:
//   B at uv + baseScroll t
//   D1 at uv + detail1Scroll t + (a + a sin(2 pi t / period), 0), with
//     detail1Sway = (a, period) (period 0: no sway)
//   D2 at uv + detail2Scroll t
//   G = (2 B.rgb D1.rgb + D2.rgb D1.rgb) (1 + B.a) intensity
// (solidenergy_ps30; the additive blend's output intensity squared in
// intensity). What it shows is G added over what is behind it, as a blended
// sheet (source alpha over) of coverage a = min(max(G) / 2, 1/2) emitting
// G / a: what is behind dims by 1 - a. It reflects nothing and is not a
// light: its emission is met only by the rays that see it (its material's
// emissive intensity is 0; it gives no estimate to the light tree). Light and
// shadow rays take it by its texture stages' alpha, as other blended sheets
// (a host keeps that low: the bridge casts no shadow in the game). Not with a
// gradient map, a portal rim, a thin film, a portal or a translucent
// material; both detail textures are named as material textures are and
// required; every value finite, intensity and period at least 0.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_SOLID_ENERGY_EXT ((remixapi_StructType)0x7fff1033)
typedef struct remixapi_MaterialSolidEnergyEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Path detail1Texture;
  remixapi_Path detail2Texture;
  float intensity;
  float baseScroll[2];
  float detail1Scroll[2];
  float detail2Scroll[2];
  float detail1Sway[2];
} remixapi_MaterialSolidEnergyEXT;

// Optional pNext on an opaque MaterialInfo: its coverage (the alpha its
// texture stages give: a decal sheet's vertex alpha) shaped per texel as
// Source's WorldVertexTransition shades a displacement's blend with a
// $blendmodulatetexture. With r and g the mask's red and green at the mask
// coordinates (the mesh's texture coordinates through `transform`: rows u
// and v of (u, v, 1)), alpha = smoothstep(g - r, g + r, alpha), each edge
// clamped to 0..1 and a step where they meet. For a displacement's second
// texture drawn as a sheet over its first, with the blend as vertex alpha:
// the edge between them breaks up as the mask has it instead of fading.
// Not with a gradient map, a portal rim, SolidEnergy, a thin film or a
// portal; the mask is named as material textures are and required (read as
// stored, repeated, bilinear); every value finite.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_BLEND_MODULATION_EXT ((remixapi_StructType)0x7fff1034)
typedef struct remixapi_MaterialBlendModulationEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Path maskTexture;
  float transform[2][3];
} remixapi_MaterialBlendModulationEXT;

// Optional pNext on a translucent MaterialInfo that makes a body (a
// measurement distance above zero, no thin wall: water): what lies behind its
// surface fades as Source's water fog fades it instead of being absorbed by
// its colour. Seen through the surface, over a length t inside the body the
// fog's share is (t - fogStart) / (fogEnd - fogStart), clamped to 0..1: the
// rest passes untinted, the fog's share is the body's own colour (its
// transmittance colour, lit), as Source's water fades into its $fogcolor.
// fogEnd (world units) above zero and finite, fogStart below it (0: from the
// surface; negative fogs the surface itself); the measurement distance is then
// not used. Light crossing the body (shadow and bounce rays) passes unfogged,
// less its Fresnel, as Source's lightmaps ignore the water. refractTint
// multiplies what passes, seen through the surface ($refracttint; {1, 1, 1}:
// none), each channel 0..1. Not with a thin film.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_TRANSLUCENT_FOG_EXT ((remixapi_StructType)0x7fff1035)
typedef struct remixapi_MaterialTranslucentFogEXT {
  remixapi_StructType sType;
  const void *pNext;
  float fogEnd;
  remixapi_Float3D refractTint;
  float fogStart;
} remixapi_MaterialTranslucentFogEXT;

// Optional pNext on a world CameraInfo beside remixapi_CameraMediumEXT: the
// body the camera is in fades what it sees as Source's water fog does (see
// remixapi_MaterialTranslucentFogEXT), untinted and linearly to nothing at
// fogEnd, into the body's colour, instead of absorbing it by that colour.
// fogEnd above zero and finite.
#define REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_FOG_EXT ((remixapi_StructType)0x7fff1036)
typedef struct remixapi_CameraMediumFogEXT {
  remixapi_StructType sType;
  const void *pNext;
  float fogEnd;
} remixapi_CameraMediumFogEXT;

// Optional pNext on opaque or translucent MaterialInfo: a thin-film coating
// whose interference colours the reflection (iridescence), evaluated
// spectrally. On a translucent material (glass, water) the surface becomes a
// smooth dielectric under the film: its reflection takes the film's colour
// whatever its coverage, and the light through it the complementary colour;
// structural does not apply. A portal material is invalid.
// It takes the place of MaterialInfoOpaqueEXT's thinFilmThickness and
// alphaIsThinFilmThickness, which alone give a film of strength 1 and
// refractive index 1.5.
// strength: 0 (no film) to 1, how much of the reflection is the film's.
// filmRefractiveIndex: 1 to 3; higher indices give more saturated colours.
// thickness: the film's thickness in nanometres, 0 to 2000. With
// thicknessTexture, the map's red channel lerps thicknessMin to thicknessMax
// instead; with alphaIsThinFilmThickness, albedo alpha times 1500.
// flowAmplitude: nanometres of animated noise added to the thickness (0: a
// still film), swirls flowScale scene units across in object space, drifting
// flowSpeed per second.
// structural: 0 to 1, how much the film's colour tints diffuse reflection too
// (structural colour on rough surfaces).
#define REMIXAPI_STRUCT_TYPE_MATERIAL_IRIDESCENCE_EXT ((remixapi_StructType)0x7fff1020)
typedef struct remixapi_MaterialIridescenceEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Path thicknessTexture;
  float strength, filmRefractiveIndex, thickness;
  float thicknessMin, thicknessMax;
  float flowAmplitude, flowScale, flowSpeed;
  float structural;
} remixapi_MaterialIridescenceEXT;

// Optional pNext on an opaque MaterialInfo with
// remixapi_MaterialInfoOpaqueSubsurfaceEXT (RTX Remix's subsurface controls):
// the share of diffuse light that scatters, 0 (none) to 1 (without this
// extension: 1), which RTX Remix has no control for.
#define REMIXAPI_STRUCT_TYPE_MATERIAL_SUBSURFACE_WEIGHT_EXT ((remixapi_StructType)0x7fff1021)
typedef struct remixapi_MaterialSubsurfaceWeightEXT {
  remixapi_StructType sType;
  const void *pNext;
  float weight;
} remixapi_MaterialSubsurfaceWeightEXT;

// Optional pNext on LightInfo (sphere, rect, disk and cylinder lights): an IES
// photometric profile, the contents of an IESNA LM-63 file (not
// null-terminated). The light's radiance in a direction is scaled by the
// profile's candela at the angle from the light's axis, the profile's
// brightest direction keeping it whole. The axis is the light shaping's
// direction, or without shaping the light's own (rect and disk: their
// direction; sphere: straight down, (0, 0, -1)); vertical angle 0 lies along
// it. Profiles are taken as rotationally symmetric: horizontal angles are
// averaged. Cone shaping still applies. Lights with the same contents share
// one profile. Text that is no LM-63 profile, or emits nothing, is invalid.
#define REMIXAPI_STRUCT_TYPE_LIGHT_INFO_IES_PROFILE_EXT ((remixapi_StructType)0x7fff101e)
typedef struct remixapi_LightInfoIesProfileEXT {
  remixapi_StructType sType;
  const void *pNext;
  const char *data;
  uint64_t size;
} remixapi_LightInfoIesProfileEXT;

// Optional pNext on LightInfo (sphere lights): a light cookie, a texture
// projected from the light along its axis (as for IES profiles: the shaping's
// direction, else straight down) like a slide projector. Within the
// projection the light's radiance is multiplied by the texture's colour times
// its alpha (so a mask may be in either), and outside it the light is dark: the texture's centre lies on the axis, its
// top toward `up` and its width spans fovDegrees (0 to 180, exclusive). The
// texture is named as material textures are (a CreateTexture hash "0x..." or
// a file path). `up` must not be parallel to the axis. Cone shaping and an
// IES profile still apply.
#define REMIXAPI_STRUCT_TYPE_LIGHT_INFO_COOKIE_EXT ((remixapi_StructType)0x7fff101f)
typedef struct remixapi_LightInfoCookieEXT {
  remixapi_StructType sType;
  const void *pNext;
  remixapi_Path texture;
  remixapi_Float3D up;
  float fovDegrees;
} remixapi_LightInfoCookieEXT;

// A private value avoids the retained 0.1003 BC5 value (139).
#define REMIXAPI_FORMAT_BC4_UNORM_EXT ((remixapi_Format)147)
#define REMIXAPI_FORMAT_BC6H_UFLOAT_EXT ((remixapi_Format)143)
// Uncompressed linear float texels (CreateTexture; a dynamic texture's
// pixels and its UpdateTextureEXT regions): RGBA, 8 or 16 bytes a texel.
#define REMIXAPI_FORMAT_R16G16B16A16_SFLOAT_EXT ((remixapi_Format)97)
#define REMIXAPI_FORMAT_R32G32B32A32_SFLOAT_EXT ((remixapi_Format)109)
#define REMIXAPI_FORMAT_BC6H_SFLOAT_EXT ((remixapi_Format)144)
