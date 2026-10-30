# USD scene capture

With `-Dusd=true`, OpenRemix writes what the host draws in one frame as a USD
capture in the RTX Remix layout. The RTX Remix Toolkit opens it, and mods
authored against it replace the game's assets through the existing mod loader
(`usd/mods.cpp`). Captures are single frames: no animation, time samples or
skeletons (a skinned instance carries its posed points; see below).

## Taking a capture

- **Mods tab → Capture → Capture frame**, or the capture shortcut. The button
  next to it sets the shortcut (as the panel's own shortcut is set). The
  shortcut works with the panel open or closed. It is kept in `state.conf`
  (`panel.sceneCaptureKey`, `panel.sceneCaptureModifiers`) and has no default.
- The request starts recording at the next Present. The frame after it is
  recorded, and its Present writes the capture on a background thread. The tab
  shows Recording, Writing, then the saved stage's path or the error.
- **Name** sets the next captures' file name (button or shortcut alike);
  empty uses `capture_<date>_<time>`. Characters a file name cannot hold
  become `_`, and an existing capture of that name is kept: the new one gets
  the time appended.
- Output: `$OPENREMIX_CAPTURE_DIRECTORY`, else `rtx-remix/captures` under the
  working directory (beside the default `rtx-remix/mods`).

## What is recorded

The draw hooks record submissions **before** mods replace them
(`api/resources.cpp`, `api/retained.cpp`), so a capture always holds the game's
own hashes:

- `DrawInstance` calls, and every visible scene instance at
  `DrawSceneInstancesEXT`, with that call's origin subtracted. Each GPU-instancing
  transform becomes its own instance. Halo sources, mask sources and masked
  overlays are skipped; CUT instances are kept.
- `DrawLightInstance` calls.
- The host's world camera (not the free camera).

At the frame's end, `usd/capture.cpp` resolves the handles against the resource
tables and copies what the writer needs. It never reads a shared material's
emission cache or a texture's upload payload, which the render thread may be
changing at the same time.

## Layout

```
capture_<date>_<time>.usda      /RootNode/{Looks, meshes, instances, lights, Camera}
meshes/mesh_<HASH>.usd          the mesh in object space; a Mesh prim per surface
materials/mat_<HASH>.usda       AperturePBR_Opacity / _Translucent shader inputs
textures/<TEXTURE HASH>.dds     DX10 DDS
lights/light_<HASH>.ies         IES profiles, as the host gave them
```

- **Names.** Mesh, material and light names are the handles' hashes: the
  `MeshInfo`, `MaterialInfo` and `LightInfo` hashes. A mod's `mesh_`, `mat_`
  and `light_` overs therefore match the same draws again (`modInstance`,
  `modLight`, and the material keys in `instances.cpp`).
- **Instances.** `inst_<MESH HASH>_<n>` instances reference
  `/RootNode/meshes/mesh_<HASH>`. Each carries its transform, its
  `remix_category:*` flags (`capture_categories.cpp`) and, on its `mesh` child,
  `doubleSided` plus the draw's alpha and blend state as
  `primvars:_remix_metadata:*`.
- **Surfaces.** Each surface of a mesh is a Mesh prim of its own (`mesh`,
  `surface_1`, ...) bound directly to its material. RTX Remix binds one
  material per mesh prim and ignores face subsets.
- **Geometry conventions.** Winding and V are flipped to the capture
  conventions that `submission.cpp` undoes on load.
- **Texture coordinates.** For repeating (and mirrored) maps, each surface's
  coordinates are moved by whole repeats toward 0. The surface samples the
  same texels, but coordinates far from 0 (hosts that bake tiling and offsets
  into them) lose precision in tools.
- **Sampler state.** `filter_mode`, `wrap_mode_u` and `wrap_mode_v` are written
  twice on each shader: as `inputs:` (what the Toolkit shows and mods
  override), and as the uniform un-prefixed `uint` attributes, with
  `enable_opacity`, that RTX Remix's own capture exporter writes. Remix's
  capture rendering reads those; without them, captured textures render
  clamped.
- **Wrap modes.** `MaterialInfo::wrapModeU/V` use RTX Remix's MDL encoding
  (0 clamp, 1 repeat, 2 mirrored repeat, 3 clip), so they are written and read
  as they are.
- **Units.** Captures are written in the host units given to
  `remixapi_ConfigureUSDModsEXT` (else 0.01 m, Y up). Loading a capture as a mod
  therefore needs no conversion.
- **Skinned instances.** These keep the mesh's bind pose in `meshes/`. The
  instance overrides its `points` with this frame's pose, as the runtime deforms
  it.
- **Lights.** Lights keep the parameters they were created with. Sphere and
  distant lights are aimed by their direction.
- **IES profiles.** A profile's text is written verbatim to
  `lights/light_<HASH>.ies` and named by UsdLuxShapingAPI's
  `inputs:shaping:ies:file`.
- **Light cookies.** UsdLux has no counterpart, so cookies use OpenRemix's own
  inputs: `inputs:remix:cookie:texture` (the cookie texture, saved with the
  others), `inputs:remix:cookie:up` and `inputs:remix:cookie:fov`.
- **Loading lights.** Loading a capture (or a mod) reads both back. A mod's
  light override keeps the overridden light's profile and cookie.

## Loading mods

A mod in `rtx-remix/mods` is composed once, without the geometry of its mesh
replacements. Each replacement (`mesh_<HASH>` and what it references) is
composed, its textures decoded, and its meshes created the first time the game
draws the mesh it replaces, off the Present thread (`Submission::OnDemand`).
The game's mesh draws until then. Memory follows what the game has drawn, not
the size of the mod. Composing every replacement of HL2 RTX's demo mod at once
holds about 50 million vertices, more than 10 GB.

A game light's replacement (`light_<HASH>`) overrides the game light with what
it authors. The lights defined under it are created the same way, the first
time the game light is drawn. As in RTX Remix, they sit in the game light's
frame: at its position, with their -Z along its direction (spot and distant
lights). They are placed as authored when the replacement has a transform of
its own.

A mod's light scale multiplies the intensity of the lights it authors: lights
under its replacements, and the intensities its light overrides set. Lights
made for RTX Remix read brighter here, mostly from exposure: the light math is
the same (color × intensity × 2^exposure). The scale is the root layer's
`customLayerData` value `openremix:lightScale` (a double, default 1). **Mods →
(mod) → Light scale** changes it for the session, and **Save** writes it into
the mod's `mod.usda`, which USD reformats.

As in RTX Remix, a texture that is missing or does not decode is left out of
its material, with a warning. The mod still loads, and the Mods tab counts the
textures it is missing. Loading a capture or a USD scene
(`remixapi_LoadUSDSceneEXT`) still fails on a missing texture.

## Textures

Textures are named by hash, so a file that already exists is not written again.
With "Keep BC textures compressed" (the default), BC blocks would leave system
memory once uploaded. **Memory → Retain texture data for captures**
(`rtx.native.capture.retainTextureData`, on by default in USD builds) keeps
them. Textures load with a level, long before a capture is requested, so the
setting has to be on by then. It costs about the textures' VRAM size in system
memory, less than the decoded copies kept with compression off. Without it, a
compressed texture is written from its preview level (at most 64 pixels), and
the capture says how many were.

Normal maps are written the way RTX Remix samples them: unsigned octahedral
in RG, as `textures/<HASH>_<encoding>_OTH_Normal.dds` with `encoding = 0`. The
Remix runtime decodes every normal map as octahedral; its Toolkit converts
tangent-space maps on ingest. Each texel is decoded as the path tracer does
for the material's normal encoding first: tangent space DX or GL, two-channel
BC5, octahedral, or Source's $ssbump. The loader maps AperturePBR's `encoding`
(0 octahedral, 1 tangent OpenGL, 2 tangent DirectX) to the runtime's.

## Tests

`tests/usd/capture.cpp` runs the whole path without a GPU: it records a frame
through the hooks, writes it, and loads it back with `usd::load`. It checks
hashes, transforms, categories, bindings, emission, verbatim BC blocks, RGBA
texels, IES profiles, cookies, the lights and the camera.
