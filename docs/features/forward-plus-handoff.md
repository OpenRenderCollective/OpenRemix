# Forward+ handoff

Current checkpoint: 2026-10-29. This page supersedes older handoff narratives;
Git history retains them. Start with AGENTS.md, this page and the
[fresh-clone setup](forward-plus-setup.md) and
[lighting architecture](../architecture/forward-lighting.md), then the
[current roadmap queue](forward-plus-roadmap.md). Historical roadmap entries are
context, not proof that an old experiment is still enabled or a defect is fixed.

## Repository and installation state

| Item | Current state |
| --- | --- |
| Native checkout | `OpenRemix/`, `forward-plus-renderer` |
| Native source checkpoint | `5ebf5f37` (handoff documentation follows this commit) |
| Native official remote | `https://github.com/OpenRenderCollective/OpenRemix.git` |
| Adapter checkout | `garrys-mod-rtx-remixed-2/`, `forward-plus-renderer` |
| Adapter checkpoint | `1c04eb07de33efdc070e933af1d16db538ca7699` (portable build helper; rendering code unchanged from `26dc7770`) |
| Adapter official remote | `https://github.com/sambow23/garrys-mod-rtx-remixed-2.git` |
| Previous playable install | Owner-local installation; not distributed in Git |
| Installed package | `forwardplus-visibility-20261027` |
| Installed native / adapter | `93c2ae0a` / `26dc7770` |

Source and playable installation are deliberately different. The material API,
Source 2 selector, normalization and packed-guide changes are not deployed.
Installed native bytes match the package above. Preserve user settings, mods,
launchers and authored cubemaps when qualifying a future package. Path tracing
remains the configured backend default; Source 2 is the preset within Forward+.

Native upstream main has three unmerged commits at this checkpoint:
`5331b2e0` (DLSS 5), `675f0c58` (USD captures), `3d4a594d` (Godot toolkit fix).
The adapter already includes its upstream main. Fetch and recheck before merging;
do not mix upstream integration into visual/performance attribution silently.
Local main branches were not moved during this handoff.

## First session for the next developer

1. Follow [setup](forward-plus-setup.md), record both Git revisions and establish
   a build/test baseline before changing renderer behavior. The guide includes
   tracked compilation helpers and packaging commands; private tooling is unnecessary.
2. Read the [architecture](../architecture/forward-lighting.md), especially the
   internal material/receiver boundary and unfinished provider selection. Source 2
   is a technique preset, not a claim of exact Source 2 behavior or a public ABI.
3. Pick a bounded task from the [current roadmap queue](forward-plus-roadmap.md).
   HDR haze and receiver/probe artifacts need a reproducible game comparison;
   packed-guide qualification and provider migration need their own focused checks.
   Do not combine visual fixes, provider migration and upstream merges in one change.
4. For performance work, establish CPU/GPU pass timings with bake completion,
   foreground status, resolution, settings and map/camera held constant. A low-quality
   smoke run or an unfocused game is not a performance baseline.
5. Keep behavior changes separate from documentation. Record what was tested and
   what remains unverified. Update the current queue when a task is confirmed fixed;
   historical entries alone should not be treated as the current state.

The clean handoff does not depend on a preserved build directory, old package or
private report. The most recent source is not the owner's older playable binary.

## What is implemented

| Boundary | Code / state |
| --- | --- |
| Evaluated material | `forward/shaders/surface.glsl`: Surface and standard PBR decoder |
| Receiver / result | `forward/shaders/receiver.glsl`: explicit receiver metadata and shading result |
| Standard shade entry | `forward/shaders/standard-shading.glsl`: `shadeStandardSurface(surface, receiver)` for world and viewmodels |
| Diffuse receiver lookup | `forward/shaders/receiver-lighting.glsl`: existing surface/probe/live-GI behavior |
| CPU ownership policy | `forward/lighting_plan.h`: resolve baked/live-GI ownership and mixed-light selection |
| Preset | `forward/presets.h`, F12 Rendering panel: Source 2 / derived Custom |
| Reflection lighting | `forward/probes/normalization.*`, `forward/shaders/probes.glsl`: shared capture-reference normalization before SSR |
| Packed transport | `forward/shaders/reflection-guide.glsl`: normal/roughness/irradiance in the existing guide |

All paths above are relative to `src/native/`. These are internal interfaces,
not a stable public shader ABI or a completed provider/plugin framework.
Local-light receiver identities/bounds are explicit; the unused localLight wrapper
was removed. Other legacy flags/experiments have not received a comprehensive purge.

Source 2 selects the existing baked surface/direct and diffuse lighting, probe
lighting for moving receivers, mixed shadows and cubemap + SSR reflections.
Applying it preserves quality, display, disk-cache, authored-volume and optional
model-chart preferences. A zero cubemap budget is re-enabled with the default
positive budget. Model charts stay enabled in default profiles, but preset
application preserves manual-density users' disabled model-chart preference.
Existing component keys persist settings; the displayed preset is derived from
them. The implementation still uses legacy controls underneath, not independent
provider enums with a general capability/resource graph.

Normalization blends capture reference luminance with the same spatial, priority
and feather weights as radiance. Explicit assignments use their own layer's
reference. World, viewmodel and water share the correction. Unknown lighting and
nonpositive references are neutral; known darkness can reduce reflected energy.
The gain ceiling is one, following the modern non-VR VRF profile. This is not all
Source 2 profiles or the rough nonmetal lightmap-SH reflection transition.

The guide is RGBA16F: octahedral normal XY, roughness Z, irradiance/pi luminance W.
Negative roughness is invalid geometry; negative luminance is unknown lighting;
zero luminance is known black. Decode through reflection-guide.glsl, never by
reading normals.xyz. MSAA selects one covered sample for all associated fields.
The separate irradiance targets and fifth storage-image write were removed.
Precision is tested after half conversion; scene appearance and FPS still need
qualification. Sparse fallback cubemap placement also no longer leaves empty
interior cells when valid capture sites are fewer than the requested budget.
That fix does not establish fitted Construct outdoor coverage.

## What remains unfinished

1. Finish independent technique/provider selection behind these contracts. Support
   providers owning several services together without double-counting lighting.
   Migrate overlapping controls deliberately; preserve existing saved settings.
2. Evaluate HLSL/Slang and single-file shader authoring. Build opt-in adapter shader
   mappings and a Refract proof first. No adapter-supplied material program loader,
   .vfx/.shader compiler, or Source shader compatibility system exists yet.
3. Reproduce severe HDR haze in the actual game with matched exposure, sky and
   materials. The isolated normalization comparison showed modest changes, not a
   fix. Forward+ and PT still use different exposure metering policies; do not
   assume shared display code guarantees matching illumination/exposure.
4. Qualify packed guides and normalization across scenes, profile bandwidth/ALU
   costs, exercise F12 interactively, and prepare a protected-settings-preserving
   package. No new default performance gain or visual parity is claimed.
5. Continue lightmap/probe quality, Construct room/seam defects, outdoor cubemap
   coverage, saturated probe-budget selection, bake times and runtime profiling.
   The complete queue and map list are in the roadmap.
6. Integrate the three native upstream commits separately, with packaging and
   backend-transition regression checks, including existing DLSS RR concerns.

Do not immediately start implementing DDGI/radiance cascades/SDF reflections to
prove modularity. First stabilize the current providers and use one real alternative
to validate the contracts. Those alternatives and virtual shadow maps are proposals.

## How to continue safely

Use the [fresh-clone setup](forward-plus-setup.md). All required build entry
points are tracked. No private build wrapper, pre-existing `_work` directory,
owner-local SDK prefix or installed game is required for the native baseline.
After enabling GPU tests, list available cases with:

```sh
meson test -C _work/build/windows --list
```

Run the oriented cubemap projection and Forward+ SSR transport/MSAA cases after
reflection changes. Use your own build directory on other platforms.

Additional relevant cases cover hardware reflection transport, viewmodel SSR,
water reflections, baked publication, live-GI/Source 2 transitions and styled mixed
lighting. Numeric sampling covers 8-512 faces, priorities/feathers, explicit
assignments, unknown/black lighting, gain ceiling and packed normal precision.
Run relevant checks after changes; do not infer moving-game performance from them.
Keep testing, profiling and deployment evidence under ignored `_work/reports/`.

Before a game/replay launch, check for existing gmod/gmod_win64/hl2/bsp-replay
processes and leave user sessions untouched. GPU timing needs foreground/focus
receipts and uncontended conditions. Static image checks do not qualify FPS, HDR
monitor output or engine material proxies. Replays need actual sky data; Construct's
procedural painted sky requires a frozen live capture. Small/no-sky smoke budgets
must not be presented as normal-quality acceptance. Do not remove caches or reports
without a concrete authorized cleanup scope.

Keep engine frustum culling off as requested until offscreen shadow casters are
handled independently. Static baked geometry should not enter moving-only shadow
passes. Moving func_brush receivers belong to probe lighting. Source-specific light
styles, shader names, VMT interpretation and classification belong in the adapter.
The current PBR/AutoPBR/replacement path remains default for ordinary materials.

## References and user assets

- [ValveResourceFormat Renderer](https://github.com/ValveResourceFormat/ValveResourceFormat/tree/master/Renderer)
  is the working Source 2 lighting reference, not native CS2 source. The reviewed
  revision is `a091a7f9dc6bdba5eae25502a20117b60e7a51bf`.
- [s&box public source](https://github.com/Facepunch/sbox-public) is an optional
  shader API reference. Neither reference checkout is required to compile.
- The adapter includes `tools/probe_editor/references/gm_construct.json`, containing
  the two authored Construct reference volumes. Using it requires the matching BSP
  fingerprint; do not bypass that check to apply it to another map revision.
- The owner's `hls2_c1p0.vmap` volume study, `hls2_content/materials`, extracted CS2
  lightmaps, RenderDoc captures and frozen live sky captures are **not in Git**.
  Request them separately if needed, or reproduce with your own game assets.
  Cubemap/combined probe volumes are the useful entities in that volume study.
- Game installations, Workshop maps, runtime settings, baked caches and previous
  playable packages are not supplied by either repository. Wider map cases include
  gm_fork, d1_trainstation_02, bm_c1a1b and combined Half-Life addon maps.

The broad roadmap goal remains paused. This checkpoint prepares a reviewable
handoff; it does not declare the renderer or roadmap complete.
