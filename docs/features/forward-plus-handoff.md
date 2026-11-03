# Forward+ handoff

Current checkpoint: 2026-11-04. This page supersedes older handoff narratives;
Git history retains them. Start with AGENTS.md, this page and the
[fresh-clone setup](forward-plus-setup.md) and
[lighting architecture](../architecture/forward-lighting.md), then the
[current roadmap queue](forward-plus-roadmap.md). Historical roadmap entries are
context, not proof that an old experiment is still enabled or a defect is fixed.

## Repository and installation state

| Item | Current state |
| --- | --- |
| Native checkout | `OpenRemix/`, `forward-plus-renderer` |
| Native modular code checkpoint | `ebfc5c85` (sun submission diagnostics; shader response split at `0c167245`) |
| Native official remote | `https://github.com/OpenRenderCollective/OpenRemix.git` |
| Adapter checkout | `garrys-mod-rtx-remixed-2/`, `forward-plus-renderer` |
| Adapter checkpoint | `952a8bf` (profiling observer-cost qualification; upstream integration retained) |
| Adapter official remote | `https://github.com/sambow23/garrys-mod-rtx-remixed-2.git` |
| Previous playable install | Owner-local installation; not distributed in Git |
| Installed package | Owner-local; not supplied by Git |
| Last restored native / adapter SHA256 prefixes | `887678bc649e` / `79d14dddcdd0` (binary hashes, not Git revisions) |

Source and playable installation are deliberately different. Recent modular
changes are not promoted to the playable install by this qualification.
Benchmark runs temporarily install candidates and restore the exact previous
binaries; they do not promote a playable package. Preserve user settings, mods,
launchers and authored cubemaps when qualifying a future package. Path tracing
remains the configured backend default; Source 2 is the preset within Forward+.

Both branches integrate upstream main as fetched on 2026-11-01: native
`51217999` and adapter `c9334fe`. The integration retains baked receiver metadata,
measured-sheet optics, authored reflection bindings and hover-state input handling.
The adapter follows the runtime-configured panel shortcut; it no longer owns F12.
Native Windows builds now require the pinned FidelityFX and XeSS submodules, and
the flat runtime packager includes their DLLs and notices. Fetch and recheck main
before further integration. The source/playable distinction still applies.

## First session for the next developer

1. Follow [setup](forward-plus-setup.md), record both Git revisions and establish
   a build/test baseline before changing renderer behavior. The guide includes
   tracked compilation helpers and packaging commands; private tooling is unnecessary.
2. Read the [architecture](../architecture/forward-lighting.md), especially the
   internal material/receiver boundary and resolved provider dependencies. Source 2
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
| Evaluated material | `forward/shaders/surface-values.glsl`: resource-free Surface; `surface.glsl` decodes PBR assets |
| Receiver / result | `forward/shaders/receiver.glsl`, `shading-values.glsl`: pass inputs and resource-free results |
| Selected shade entry | `forward/shaders/shading-model.glsl`: `shadeSurface(surface, receiver)`; default aliases the standard model for world and viewmodels |
| Diffuse receiver lookup | `forward/shaders/receiver-lighting.glsl`: selected private contexts; `diffuse-composed.glsl` combines baked direct and incoming |
| CPU ownership policy | `forward/lighting_plan.h`, `provider_plan.h`: energy ownership, capabilities and resource dependencies |
| Sun visibility resources | `forward/shadows/sun_maps.h`: image, layer targets and frozen-projection cache; renderer schedules enabled cascades and transition clears |
| Techniques / preset | `forward/techniques.h`, `forward/presets.h`, Rendering panel: component selectors and Source 2 / derived Custom |
| Reflection lighting | `forward/shaders/reflection-environment.glsl`: source-owned cube/sky fallback; `reflection-transport.glsl`: current-frame radiance/confidence |
| Packed transport | `forward/shaders/reflection-guide.glsl`, `reflection-guide-reference.glsl`: geometry packing and paired normalization-domain adapter |
| Baked storage | `forward/shaders/baked-storage.glsl`: guarded field declarations imported by their query sources |
| Local storage | `forward/shaders/local-light-storage.glsl`: default records/clusters shared by source and builder |
| Capture ownership | `forward/probes/capture_publication.h`, `capture_providers.h`: retained assets, admission and standalone lifecycle |

All paths above are relative to `src/native/`. These are internal interfaces,
not a stable public shader ABI or a completed provider/plugin framework.
Local-light receiver identities/bounds are explicit; the unused localLight wrapper
was removed. The [controls catalog](forward-plus-controls.md) classifies the
main native Forward+ environment controls and compatibility keys. No blanket
purge is implied; shared native/adapter controls and dynamic names still need
separate review.

Source 2 selects the existing baked surface/direct and diffuse lighting, probe
lighting for moving receivers, mixed shadows and cubemap + SSR reflections.
Applying it preserves quality, display, disk-cache, authored-volume and optional
model-chart preferences. A zero cubemap budget is re-enabled with the default
positive budget. Model charts stay enabled in default profiles, but preset
application preserves manual-density users' disabled model-chart preference.
Existing component keys persist settings; the displayed preset is derived from
them. Typed component selections now drive presets and UI through those existing keys.
A bounded capability/resource plan and standalone capture lifecycle are implemented.
The default Source 2 recipe still coordinates diffuse/capture finishing through
one job and transport budget. It is not a general runtime provider registry.

Normalization blends capture reference luminance with the same spatial, priority
and feather weights as radiance. Explicit assignments use their own layer's
reference. World, viewmodel and water share the correction. Unknown lighting and
nonpositive references are neutral; known darkness can reduce reflected energy.
The gain ceiling is one, following the modern non-VR VRF profile. This is not all
Source 2 profiles or the rough nonmetal lightmap-SH reflection transition.

The guide is RGBA16F: octahedral normal XY, roughness Z, irradiance/pi luminance W.
Negative roughness is invalid geometry; negative luminance is unknown lighting;
zero luminance is known black. Decode through reflection-guide.glsl and
reflection-guide-reference.glsl, never by reading normals.xyz. MSAA selects one covered sample for all associated fields.
The separate irradiance targets and fifth storage-image write were removed.
Precision is tested after half conversion; scene appearance and FPS still need
qualification. Sparse fallback cubemap placement also no longer leaves empty
interior cells when valid capture sites are fewer than the requested budget.
That fix does not establish fitted Construct outdoor coverage.

## What remains unfinished

1. Qualify additional provider recipes against resource/admission and reflection
   reference contracts. Existing query sources own their shader resources;
   captures have separate publications/caches and a standalone lifecycle. The
   default recipe now owns its combined finisher separately from capture scheduling,
   while retaining shared transport/admission. Frozen reference identities and
   binding-time neutral metadata views are implemented for default sources. Mixed
   identities neutralize the whole image; further component recipes need their
   own qualified domain contracts. The default shared budget is intentional;
   do not create independent jobs merely to make every technique run in parallel.
   Retain work reuse and avoid duplicate energy.
2. Later: evaluate HLSL/Slang and single-file shader authoring. Build opt-in adapter shader
   mappings and a Refract proof first. No adapter-supplied material program loader,
   .vfx/.shader compiler, or Source shader compatibility system exists yet.
3. Reproduce severe HDR haze in the actual game with matched exposure, sky and
   materials. The isolated normalization comparison showed modest changes, not a
   fix. Forward+ and PT still use different exposure metering policies; do not
   assume shared display code guarantees matching illumination/exposure.
4. Qualify packed guides and normalization across scenes, profile bandwidth/ALU
   costs, exercise the configured panel shortcut interactively, and prepare a protected-settings-preserving
   package. Recent matched Crossfire checks preserve the checked output but show
   a small positive armed CPU recording residual. An exact-binary repeat reversed
   that difference, so no stable regression or zero-overhead conclusion is established.
   Matched Construct/Crossfire comparisons now cover the latest host ownership
   and resolved-plan changes, with no consistent CPU/GPU regression in the checked
   workloads. Neutral metadata still adds one aligned header per nonempty image.
   Additional recipes, hardware and scenes require their own qualification; no
   universal zero-cost claim or new default performance gain is implied.
5. Continue lightmap/probe quality, Construct room/seam defects, outdoor cubemap
   coverage, saturated probe-budget selection, bake times and runtime profiling.
   The complete queue and map list are in the roadmap.
6. Qualify the integrated upstream features in game, including camera screens,
   fog, material extensions, screenshots and upscaler/backend transitions.

[PR #37](https://github.com/OpenRenderCollective/OpenRemix/pull/37) is the real
alternative used to exercise the contracts: Radiance Cascades, including explicit
baked-direct plus live-incoming composition. Its adapted code checkpoint is
`bae33938` on the contributor's `radiance-cascades` branch. It is **unmerged**;
wait for the owner's explicit merge clear. The main branch does not contain the
cascade implementation. Do not deploy it or start another GI prototype merely
to demonstrate the API. The public HLSL/single-file loader remains future work.

Replacement compilation exercises actual world, metal, viewmodel, live, water
and resolve passes with default storage/implementations physically absent.
Numerical GPU checks cover mixed shadows, styles, cache and provider transitions.
Shader-equivalence checks are evidence about checked work, not universal FPS
acceptance. Current shader variants are byte-identical to the material-response
checkpoint for 17 main and 23 adapted-PR cases; only a sampler rationale comment
differs in those shader trees. Consolidated Crossfire/Construct comparisons preserve the checked
output/settings/draws and have overlapping GPU timing ranges. The latest frame
snapshot also has an isolated Crossfire comparison. Earlier large armed CPU
residuals were not reproduced by the fixed-warm-up follow-up. The latest host
ownership/plan changes now have fixed-warm-up ABBA checks on both maps:
content/settings/draws and the checked world crop match, and repeated CPU/GPU
ranges show no consistent regression. This closes the current split's matched
runtime gate on those workloads, not a guarantee for all hardware or future
providers. Crossfire was capped at 200 FPS; Construct remained roughly
115-129 FPS. 300 FPS at 1920x1080 and the broader runtime optimization queue
are not achieved.

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

Work resumed on 2026-11-01; this 2026-11-04 checkpoint covers modular provider
migration, resource ownership and controlled performance diagnosis. This does not declare the renderer or roadmap complete.
