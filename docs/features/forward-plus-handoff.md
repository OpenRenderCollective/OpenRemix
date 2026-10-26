# Forward+ continuation guide

Current branch checkpoint (2026-10-25): runtime profiles enable eligible static
model lightmaps by default (`c819cf30`); `rtx.native.forward.lightmapModels=0`
disables them, while dynamic objects keep probe lighting. Base-only surface
references separate aggregate base lighting from styled components. This source
change has normal-quality multi-map/cache and private live-game qualification;
remaining probe, seam, noise, bake-time and broad performance work stays open.

Opaque sun/local shadow alpha work is reduced (`58571eda`): ALWAYS bypasses
albedo sampling and NEVER discards immediately, preserving conditional alpha.
GPU shadow regressions pass and existing 156 deterministic overlay images remain
exact. A live Construct pair has lower weapon sun-shadow cost, but world/unarmed
variation prevents accepting its entire whole-frame difference as a shader gain.

The isolated weapon depth prepass (`97bfb6af`) remains a private opt-in via
`REMIX_DEBUG_VIEWMODEL_DEPTH_PREPASS=1`, with alpha/MSAA/SSR correctness coverage.
It is not an accepted default FPS improvement. CS2 capture evidence confirms
early weapon depth and later world colour rejection; our world coverage masking
is still pending. No temporary RenderDoc test hooks remain in source.

The playable installation now includes these qualified default changes from
`forwardplus-static-models-20261025`: native `20d5f30f`, adapter `b04b344c`.
All 157 owned files verify and 101 protected settings/assets remain unchanged.
The isolated weapon depth prepass stays off. An installed normal-quality
Construct check with actual captured sky and no model override generates 1407
model charts, loads compatible cached lighting and matches the qualified fixed
image exactly, without validation/synchronization diagnostics. This is not
gameplay FPS acceptance or an installed path-tracer RR qualification.

Quality-reference caveat: incoming-ray hemisphere references omit analytic
receiver-direct lighting. Comparisons against fully baked aggregate surface or
probe fields therefore need a component audit before their error measurements
or inferred placement policies can be trusted. Use the offline `surface`
reference format for normal-bearing surface irradiance with the bake's direct
policy; it preserves SH and reconstructs irradiance at the supplied normal.
This diagnostic correction does not change rendering defaults or existing bakes.

Current testing installation: `forwardplus-static-models-20261025`, native `20d5f30f`
and adapter `b04b344c`. All 157 deployed files verify, and 101 protected files
(settings, launchers, authored bounds and replacements) remain byte-identical.
This includes upstream integration, separate pinned DLSS CNN/Transformer packaging
and redundant consecutive Forward+ draw-binding elimination, plus bounded receiver
range specialization. The proxy DLL, native DLL and runtime manifest changed in this refresh.
Eligible static model charts now follow the runtime default; selected-probe
batching and the isolated weapon depth prepass remain opt-in. Earlier deployments below are
historical. Fixed-view checks do not establish moving-gameplay FPS or final quality.

The earlier combined installed smoke produced Forward+ image and timing/report outputs. Its
Construct procedural sky was missing, so this is functional coverage rather than
live-game lighting parity. The subsequent path-tracer stage produced no final
capture and reported two synchronization write hazards inside DLSS RR resource
clears. No replay process remains; its final exit code was not retained. Do not
describe that combined run as passing or attribute the hazards to first-party
code without isolating them. Repeat with and without RR under matched conditions.

Quality replay captures must supply actual map sky data. The quality preset now
requires a usable sky; Construct's procedural `painted` sky needs a frozen live
capture. Missing-sky diagnostic captures remain useful for controlled algorithm
comparisons, but cannot qualify the game's lighting appearance. Normal sample
budgets alone are insufficient: record sky, exposure, materials, bake completion,
and experimental coverage as well. Existing noise/seam/probe issues remain open.
The required-sky guard rejects missing Construct sky before any presentation.
A fresh normal-budget installed Forward+ run using a validated frozen live sky
then completes bake, authored capture readiness and final image/report publication
without validation/synchronization diagnostics. This corrects that test's missing
illumination; it does not establish that noisy probes, seams or all rooms are fixed.
The latest installed Forward+ check uses that captured sky and matches the prior
installed image exactly, with no validation/synchronization diagnostics. Runtime
sample budgets, model-chart eligibility defaults and protected user assets stay intact.

The branch also specializes bounded consecutive receiver ranges within partially
charted opaque draws. Coverage, original triangle order and bake compatibility are
preserved; fragmented meshes retain a single generic draw. Static model charts
are now a runtime-profile default on the branch. Continue their wider-map quality/cost qualification separately from
dynamic-prop volume support; static model gaps can be much smaller than a useful
volume interpolation cell. Rejecting all blocked stencil contributors alone is
not a complete lighting solution when no clear support remains.

Current probe-batching executable diagnostics show generic world fragments growing from about
110 KiB to 135 KiB and viewmodel fragments from about 156 KiB to 181 KiB; the fully mapped
lightmap variant remains 105728 bytes. Both variants report 128 registers. This narrows the
hallway regression investigation to generic receiver shader work rather than a universal
mapped-world cost; it does not establish causality or literal spill memory. Cluster lists
are emitted in sorted light order, and the stencil cache is shared by both paths. Preserve
unknown-coverage fallback and directional corrections when prototyping a smaller receiver
reconstruction; measure its cost before changing defaults. Both diagnostic relay runs pass.
Two smaller receiver reconstructions are rejected and removed from production: vector validity
simplification passes GPU reference checks but saves only about 128 bytes; scalar gathered
lookups pass correctness/image checks yet lose about 2.1-2.2% whole-GPU time in opposite-order
128-frame hallway/room relay comparisons. Scalar partial-lightmap code reports 161 registers
versus 128, despite slightly smaller binaries. These are relay checks, not gameplay FPS.
Keep the original vector reconstruction and its missing-data/directional fallback policy.

A fresh focused Construct off/on/on/off comparison on the deployed `b8c384a` DLL confirms the
selected-probe batching tradeoff: white-room whole-GPU mean of per-run medians drops from
9.787 to 8.297 ms (15.2%), while hall-east rises from 4.103 to 4.213 ms (2.7% slower)
and the other hallway stays approximately unchanged. White-room opaque cost falls from
7.177 to 5.859 ms. All four runs retain the same bake hash, confirmed Windows/game focus,
DLL hashes and protected settings. These are fixed-view measurements with full-resolution
SSR and 4x MSAA, not moving-gameplay FPS acceptance. First-pair upper-world image differences
are at most 3 channel values in hall-east and 1 elsewhere; exposure/animation prevent a broad
pixel-identical claim. The gathered field is about 2 MiB and is reused after completion.
Keep batching opt-in; investigate receiver-level cost and hallway regressions before any
default change, and retain the moving-gameplay/wider-map qualification tasks.

The 2026-10-24 testing refresh deploys native `b8c384a` and adapter `a499842` from the immutable
`forwardplus-reflection-only-20261024` package. All 155 owned files verify; only the native DLL,
upstream license and runtime manifest changed. Ten protected settings/assets/launcher files
are byte-identical, including authored Construct cubemap bounds. Staged/installed Construct
replays differ by at most one display-channel value, with only the known older Vulkan-layer
device-structure warning and no synchronization hazards. Rendering defaults remain unchanged;
selected-probe batching and sparse sun depth remain opt-in. Earlier refresh details below
are historical validation of the retained optimizations.
One installed relay run completed image/timings but timed out waiting for the fresh reflection
report within 60 presents. A repeat exited successfully with that report and matching image.
Keep this intermittent harness-report timeout on the diagnostic todo; do not count the first
run as a full smoke-test pass.
Adapter diagnostic-host commit `6cbb0b3` replaces timestamp-only probe-report freshness with
retirement of the previous owned response before each request, followed by a nonempty-file
check. A filesystem regression covers identical replacement timestamps/content, stale/empty
responses and request-creation errors. Five cached Construct repeats return fresh published
reflection reports with stable bake identity. This hardens the host without establishing the
exact cause of the earlier one-off timeout; keep watching for it. Only tools/tests changed,
so the installed adapter DLL remains the verified `a499842` package.

## Start here

The separate GMod testing install was refreshed on 2026-10-23 to native `d9d54c2` and adapter
`a499842`. The latest refresh enables early-depth shading only for viewmodel draws whose packed
state guarantees unconditional coverage, and includes ordinary-water shader parity and newer
profiling diagnostics. Cutouts and explicit camera-backface rejection retain ordinary shading.
The world early-depth optimization and independent reflection influence/feather sidecars remain.
Sparse baked sun depth and selected-probe batching remain opt-in; deployment enables neither.

All 155 owned files verify. Only the two DLLs and runtime manifest changed; the other 152 owned
files and ten protected settings/assets/launcher files verify unchanged, including authored Construct
boxes and addon mounts. The root install record agrees with the immutable package checkpoint.
Main branches remain upstream and rollback copies remain available.

Six final GPU suites pass validation/sync checks across world/MSAA, viewmodel overlay/shadows,
baked probes, SSR, environment materials and animated frame slots. Perspective camera-culling tests
cover reversed winding and explicit two-sided policy. Separate invocation queries confirm less
viewmodel fragment work with unchanged passing samples and geometry counts. Focused opposite-order
Construct timing checks improve both eligible-viewmodel repeats. The staged and installed DLL replay
outputs agree with the control within one display-channel value; native-runtime validation retains the
known older-layer device-feature-structure message, with no new synchronization/descriptor report.
These are bounded functional/fixed-view checks, not broad moving-gameplay or map-quality acceptance.

Ordinary lighting cache identity remains unchanged by this refresh. Optional sun-depth sidecar
policy/layout changes can require fresh depth capture while retaining cached irradiance. Older
pre-codec caches remain on disk. Automatic authored fallback stays off by default; use the rendering
menu for targeted testing. User backend settings are preserved.

This checkpoint also includes chart-edge denoising, stable progressive history, surface-hemisphere
transport, cosine-anchored surface irradiance and chart-local exterior donor repair. The reproduced
Construct ceiling-fixture halo is removed in neutral, raw and shaded fixed-view checks. Hidden interior
samples still consume bake/atlas capacity; the repair prevents them donating filtering energy rather
than reclaiming their allocation. Connector/secret-room and broader map quality remain open.

The depth prepass now skips texture sampling for constant alpha comparisons. All eight comparisons
retain world/viewmodel coverage across draw order and MSAA. Fixed-view Construct GPU comparisons
improve about 3–4%; a Crossfire pair improves about 1.7%. Installed-DLL and viewmodel functional tests
pass with synchronization validation. These measurements do not establish live armed/moving FPS.

This is an experimental testing checkpoint, not broad gameplay or probe-quality acceptance.
Adaptive irradiance runs automatically when eligible in this checkpoint. Continue the requirement
matrix in [the roadmap](forward-plus-roadmap.md) and [the renderer guide](forward-plus.md) before
claiming a release-ready default. Preserve static visibility, unknown coverage, controllable groups
and offscreen moving shadows. The prior uniform-grid checkpoint was `e51bff0`.

## Current profiling direction

The branch now merges official OpenRemix main through `877df12`, including its Apache-2.0 license
file and reflection-only third-person player categories. The adapter main was already contained.
Compatibility-raster conflicts preserve both upstream primary-visibility rejection and Forward+
ownership, so owned draws are not duplicated by fallback. Path-tracing shaders are regenerated from
the merged source; bounded regeneration and source/binary checking pass, along with scene decoding,
retained replay, TLAS masks and the world/viewmodel raster regressions. `generate.py --jobs N` now
bounds GLSL compilation/validation concurrency for large optimized backends.
Forward+ now separates primary ownership from transport ray purpose: reflection-only bodies
remain in bounce transport but are rejected by visibility/depth queries and surface charts.
CPU scene/cache ownership checks and the hardware transport suite pass, including emissive
reflection hits and geometry-query exclusion. World/viewmodel primary visibility toggles pass
at 1/2/4 MSAA; toggling retained body categories removes/restores the moving sun shadow without
changing the baked-world hash. Local-shadow/cache and viewmodel isolation regressions also pass
with Vulkan/synchronization validation. Ordinary map cache keys are preserved.
Live reflection resolve now passes category toggles, movement, stale-hit removal and unchanged
acceleration reuse across all 1/2/4 MSAA and reflection-resolution combinations with validation.
The integration is deployed as native `b8c384a` / adapter `a499842` in the testing install.
Do not remove these instances from all transport.

The viewmodel continuation adds a default early-depth variant only for packed ALWAYS-alpha draws
with camera backface rejection disabled. Its shader contains no discard instructions; cutouts and
explicit camera culling retain the ordinary shader. This needs no second geometry pass or new target.
Six GPU suites cover alpha comparisons, draw orders, MSAA, perspective camera culling, overlay/world
shadows, baked probes, SSR, environment materials and animated frame slots. A focused opposite-order
Construct comparison improves both viewmodel repeats, while separate invocation queries show fewer
shader invocations at unchanged passing-sample/geometry counts. These are fixed-view results, not
whole-roadmap gameplay acceptance. The private disable override is documented in the renderer guide.
This continuation is deployed in the verified testing refresh above; broader gameplay and roadmap
quality gates remain open.

The selected-light visibility gather and receiver integration are now opt-in through presence of
`REMIX_DEBUG_PROBE_BATCH` (remove the variable to disable). `forward/probe_batch` produces exact,
bounded probe-major vector groups; `shaders/probe-batch.glsl` reconstructs one group with the existing
stencil. Frame owners retain derived fields through their fences, including source/pipeline ownership.
Pipeline preparation runs on the existing worker. Directional moving corrections keep sparse attributed
lookup, unsupported fields keep sparse fallback, and water/live GI keep their existing paths.
The focused Construct colour-room comparison improves in both opposite-order repeats with unchanged
light budgets and bake identity. Fixed room regions match; animated viewmodels prevent whole-screen
pixel equivalence. The ordinary hall shows no clear benefit. Before enabling by default, test moving selection churn,
wider maps, preparation cost and peak residency under churn, and
resolve that hall tradeoff. The old validation layer reports the same unrecognized device-feature
structure on/off; no new synchronization/descriptor error appears, but full runtime validation remains
qualified by that compatibility message. Test-install bytes/settings were restored after owned runs.
The roadmap's broader quality and deployment gates remain open.
A receiver GPU fixture now compares the actual vector helper with both sparse GLSL and an independent
CPU reconstruction for both sparse encodings and directional wrappers. It covers weighted holes on
both sides of the tiny-weight cutoff, unsupported/insufficient stencils, 8/16 corners, group changes,
stencil revisions and the full 64-light selection. The Crossfire opposite-order replay is pixel-identical
with neutral timing; it exercises 456 lights with 16 selected highlights and 4 mixed slots, without
establishing live gameplay performance. Diagnostics now expose active field, resident and peak buffer
bytes, and a separate GPU preparation range. Peak accounting retains old submitted/abandoned owners
until released; the receiver fixture checks this independently of current resident bytes.
A 64-frame scripted Construct camera transition retains its bake identity; all five sampled images
match the feature-off run within one display-channel value. It records about 2 MiB active/resident
field storage and 5 MiB peak residency during selection changes. A fixed-view replay records a 4 MiB
peak. These are measured cases, not a universal bound or an armed moving-gameplay acceptance. The
preparation timer is absent from the completed steady-state timing window because reuse emits no
gather; measure first use and sustained selection churn separately before judging preparation cost.
Live frame-profile CSV now appends gather presence/time and timing truncation, preserving previous
columns. A subsequent live churn comparison was interrupted by an access violation in the original
Source shader DLL's asynchronous loading stack. Its cause remains unproven; qualify this before
accepting sustained gameplay/churn performance. The two observed preparation ranges preceded
Windows-confirmed focus and are excluded from performance conclusions. Short alternating holds
also did not outlast highlight fade, so repeat with adequate dwell and verify actual selection changes.
Both original test-install DLLs and protected settings were restored after the owned session ended.
Disassembly narrows that host crash to an indirect call through an overwritten static device-object
vtable pointer in the original shader DLL. Pixel-like pointer bytes are evidence of corruption, not
proof of its writer or cause. A self-tested external hardware write watch now supports exact owned
process reproductions without patching application code/data. One watched transition session did not
reproduce the overwrite or crash; it is not a fix or a performance acceptance.
Longer watched feature-off/on controls now complete eight transitions with no observed header write.
The new selected-ID fingerprint confirms changing sets; recorded-gather and completed-reuse counters
separate producers (including cancellation) from reuse. Focused, untruncated gather intervals are
about 0.110 ms at first use and 0.011-0.016 ms thereafter in this diagnostic session. Debugger presence
and asynchronous snapshots qualify these values: they are not normal moving-gameplay FPS/stutter
acceptance or a measured CPU allocation cost. Counters/fingerprints do not alter exact cache keys.
The test harness's Lua quit is blocked; a visible-window close request also did not establish orderly
engine shutdown. Keep bounded owned-process cleanup, but verify the actual engine close path in
future shutdown tests. Both installed DLLs and protected settings were restored after these controls. Retain the stability
follow-up and use feature-off controls before changing production resource or shadow lifetime rules.

The installed runtime has completed Windows-confirmed foreground Construct samples with the
physgun, full-resolution SSR and stable bake identity. Source's own focus flag alone proved
insufficient, so future runs must check Windows process ownership and visually verify the camera.
The colour-room view is trace-confirmed in non-solid space, facing `GM_CONSTRUCT/COLOR_ROOM`
on a `func_brush`. Its probe-lit shell is substantially more expensive than the lightmapped hall.
Bounded receiver instrumentation also shows mapped geometry invoking shaders despite zero passing
depth samples, without omitted query ranges. The world prepass path now selects explicit early-test
shader variants with depth writes disabled in its shaded pass. Coverage/depth remain owned by the
matching alpha-tested prepass. Other cameras, terrain, water, prepass-off and shadow shader policies
retain their separate handling. Six GPU suites pass synchronization validation; opposite-order live
Construct and full-resolution Construct/Crossfire replay checks show a benefit at stable bake identity.
Stationary brush bake membership and the remaining probe shading cost are next; no static-brush
policy has changed. A subsequent lazy coarse-stencil experiment matched the reference but traded
white-room improvement for a hall regression, so its production changes were removed. The additional
GPU regression for wholly unsupported fine records remains. Construct's room-shell brush submodels
allow material/color edits and disable baked shadow casting; preserve those policies before publishing
them as stable bake receivers. These frozen-view checks do not establish broad gameplay FPS parity.
Further live diagnostic ablations retain one bake while independently disabling highlights and mixed
selection. They identify substantial per-light receiver cost beyond aggregate probe reconstruction;
these quality-changing runs do not justify lowering the defaults. Investigate bounded visibility lookup
addressing next, retaining exact values, unknown coverage, directional ownership and sparse fallback.
A bounded byte-rank prototype has since matched reference data but failed the white-room performance
check; its production changes were removed. The wider256-light GPU lookup fixture remains. Compiler
capture identifies high world/viewmodel fragment register use, but implausible local-memory values
must not be reported as literal byte allocations. Investigate stencil layout/lifetimes next and verify
both reconstruction and normal-runtime performance. Missing prepassed/viewmodel pipeline labels are
registered on the branch for future captures.
The follow-up vector/fixed-component stencil layouts and offline Forward+ SPIR-V pass were also
removed after checks failed to establish a useful performance gain. Production stencil/compiler
policy is unchanged. The next reference-guided study is batched selected-light visibility reconstruction:
the pinned viewer samples baked receiver channels once before its per-light evaluation. This does not
justify removing specular highlights. A derived vector field must preserve exact masks, unknown
coverage, attributed corrections, frame lifetime and sparse fallback, without touching diffuse bakes.
Include preparation and storage overhead in the comparison.
The branch's ordinary-water embedded fragment is now regenerated to match maintained sources; four
water GPU suites and source/binary validation pass. That parity correction is not yet deployed, and
native `ac33981` remains the installed checkpoint.

## Recent optional work

- `forward/bake/sun_depth*`, `controller_sun_depth.cpp` and `forward/shaders/sun-depth.glsl` own the
  sparse static-sun field for unmapped world receivers. Capture writes final GPU tile addresses;
  fences guard publication, readback and restore. Compact pages preserve samples, and local filter
  coordinates keep results invariant under repacking. Transmission and missing coverage use the
  existing fallback. The test-only soft filter is not integrated: finite-source/subtexel coverage
  error remains a default-blocking quality gate. Keep viewmodel and moving-shadow paths separate.
- `remix_reflection_ext.h`, scene reflection volumes and probe authoring accept optional sampling
  metadata without changing version-one records. Adapter `cubemap_overrides.*` accepts `BSPCUBE2`;
  editor document/probe-report import preserves supported influence and feather fields. Independent
  influence gizmos remain unfinished. `probe_editor.bsp_reflections` extracts raw map-fingerprinted
  projection references, including Black Mesa Hammer side assignments; it does not publish them or
  infer receiver influence/priority. Resolve those semantics before enabling runtime BSP import.

## Module ownership

- `forward/probes/priority_order.h` and `image_data.*`: canonical captures/cache data retain their order;
  upload records and every image mip sort together by priority. `ImageAsset::header()` exposes the
  physical order for diagnostics and CPU viewmodel masks. Do not mix canonical indices with layer bits.
- `forward/probes/data.cpp` and `forward/shaders/probes.glsl`: matching priority-layer feathering.
  Equal-priority sets retain one traversal; mixed sets normalize edge coverage across priority layers,
  cancelling shared floor/wall bands. An isolated Construct glossy-boundary check confirms the blend
  and unchanged control interiors; a moving-path validation smoke passes. This continuation is now
  in the testing install. A controlled 33-frame glossy traversal confirms a less abrupt transition
  with matching endpoint interiors; sharp common reflection-detail changes still need investigation.
  Wider maps, material/roughness variants, real gameplay and performance acceptance remain open. Authored sets
  replace automatic sets by default; explicit fallback composition is implemented and available for
  targeted testing. It preserves empty-set/reset semantics and reports capacity omissions.

- `forward/Renderer::activate`: queue-idle adoption publishes retained bake metadata and owned
  allocation totals before asynchronous queries can observe the replacement renderer. Preparation
  workers do not read a controller owned by the active renderer. Frame completion uses the same
  publication helper; a real unload still clears the controller and published lighting.

- `forward/shaders/cluster.comp`: each workgroup shares batches of light spheres while retaining
  sorted prefix/tail membership and independent overflow counts. Inactive lanes must reach both
  shared-memory barriers; guard writes rather than returning before a barrier. No persistent cluster
  result is reused, so camera/light changes are evaluated on every frame.

- `forward/shadows/sun_cache.*`: frozen sunlight ownership and projection reuse. Subtexel fit
  changes retain the exact cached matrix for both static sampling and moving-caster rendering;
  significant movement, scene replacement and resolution changes still invalidate. This does not
  remove uncovered-receiver fallback cascades. Broad live shadow-quality acceptance remains open.

- `forward/bake/adaptive_grid.*`: coarse placement and receiver/reference-driven refinement selection.
- `receiver_support.*`: bounded receiver support, halos and deterministic neighbour collection.
- `refinement_builder.*`: coverage-first regional subdivision, canonical shared physical records and compaction.
- `grid_directory.cpp`: directory validation, ownership and reciprocal compatible-face flags.
- `grid_sampling.cpp`: CPU reference reconstruction and coarse/fine transition weights.
  Broad six-axis moments may reduce weak coarse fallback below its nominal blend share, but
  cannot promote it over valid fine support. Directional-depth confidence retains its symmetric policy.
  The shared GPU stencil applies the same rule; placement, storage and cache identity are unchanged.
- `controller_refinement.cpp`: asynchronous refinement scheduling between coarse/required surface and fine work.
- `adaptive_grid.cpp` / `sparse_grid.cpp`: an empty coarse seed retries ordinary full-budget placement;
  an empty density-search trial keeps the last verified field. `EmptyProbePlacement` is distinct from
  cancellation or malformed inputs. A wholly unusable partition still rejects.
- `shaders/grid-stencil.glsl`: matching shared stencil for diffuse, sky, sun and local visibility/corrections.
- `probe_visibility_plan.cpp`: conservative per-record light volumes; adaptive records keep their largest
  referencing/trace support, while shared coarse corners retain coarse reach.
- Existing cache/controller/publication modules retain lifetime, cancellation, fencing and validated import.

The current bake algorithm policy revision is 30; the handoff prototype used 24 and the previous
uniform testing build used 23. Grouped probe policy version 2 also separates caches with incomplete controllable
groups. Directional probe depth remains opt-in and disabled by default. Upgrading deliberately rebakes once;
camera movement and runtime toggle values
do not change authored lighting identity. GPU publication packs the mode and
directory length into raw bits of header settings.z; CPU/cache metadata retains numeric modes. Do not
use old private binary parsers without adapting that distinction. Public Remix ABI layouts are unchanged.
Styles, local unit-direct fields and visibility use the same physical record ownership. Do not merge
unknown or unsupported contributions into permanent aggregate lighting.

## Build and checks

Follow repository `AGENTS.md`. Use Visual Studio x64 developer PowerShell, clang-cl, Meson/Ninja and
Vulkan SDK. `scripts/windows/build-remix.ps1` is the portable Windows entry point; it builds dependencies
and runs tests. Existing local configured builds can use `meson compile -C <build> remix` and
`meson test -C <build> --print-errorlogs`. GPU suites require supported hardware and their build option.
The build generates and validates GLSL-derived SPIR-V with the pinned shader toolchain,
including Forward+, live GI, water and the GPU fixtures. Generated headers belong in the
build directory and are not committed. `generate.py nrd --check` and `fsr31 --check`
verify the separately committed DXC binaries; `--validate` is an alternative for those two
backends. Do not use these modes for GLSL backends.

Relevant suites include adaptive ownership/energy/transitions, per-region GPU addressing/styles,
shared-face biased queries, cache integrity, hardware transport, aggregate/mixed corrections,
surface lightmaps/MSAA, lighting-component diagnostics, SSR and controllable direct/indirect groups.
The adapter's default-off replay fixtures have their own settings tests. Broader editor/live input and
moving actor acceptance remain open. Existing tests passing is not a blanket release approval.

**Incremental-build trap:** do not restore source/header backups with old modification times. This
produced stale GPU-struct objects and native access violations. Update restored file timestamps or
rebuild all affected objects, then relink both runtime and tests. Never overwrite a running DLL/executable.

## Diagnostics and performance discipline

For controlled GMod transport references, the adapter's private
`OPENREMIX_PROXY_BAKE_SKY_REFERENCE` variable can pin a validated `.sky` capture. It is read-only
and rejects an invalid reference rather than replacing it with live pixels. Unset it for ordinary
play; normal capture-noise reuse keeps its existing bounds. Compare input fingerprints including
the dome before using a reference to judge placement. Match transport sample counts on original
and proposed positions, and reconstruct visibility as well as SH; raw point brightness alone is
not an interpolated receiver-quality result.

For bounded point-transport references, set `REMIX_DEBUG_BAKE_REFERENCE_POINTS` to a UTF-8 text
file containing a sample count (64..8192), followed by at most sixteen XYZ world positions. The
controller bypasses cache loading for that diagnostic run and traces unbiased incoming SH through
the existing immutable transport, separately from the placement grid. Fenced readback writes
`<input>.results.json` with scene hash, sample/bounce counts and combined-lighting status. Its RGB
coefficients use the same cosine convolution as probes. Primary sunlight remains a separate runtime
term; primary local contributions depend on the transport's direct-light membership, so this is not
an unconditional reference for the entire final shaded pixel. Use an isolated bake cache with the
same canonical sky reference for comparisons. No request means no diagnostic rays or changed cache
policy. This does not modify probe origins, surface charts, cache identity or the public API.

Prefix the input with `hemisphere` to request an independent cosine-weighted radiance integral:
sample count, then XYZ/unit-normal sextuples. The output mode is `hemisphere` with per-site
`diffuseIrradiance` (linear E, not E/Pi); ordinary requests retain mode `sh` and RGB coefficients.
Rays use deterministic stratified directions and bounded 256-ray submissions. This integrates
environment/material radiance and reflected lighting; primary analytic local lights and sunlight
must be compared separately. It is not a replacement for a fully composed image reference.

Lighting view `Bake coverage` (`rtx.native.forward.debugView=6`) shows opaque atlas receivers blue,
valid shared probe stencils green and uncovered receivers red. Sky/water/alpha/terrain composition can
mix display colours. View zero restores normal shading and does not intentionally invalidate the bake.
`REMIX_DEBUG_FORWARD_RECEIVER_TIMINGS=1` times consecutive receiver categories in original draw order.
It adds timestamps; use normal-order image controls and check truncated ranges. Diagnostic counters and
rendered spheres are not live gameplay acceptance.

`REMIX_DEBUG_FORWARD_REFLECTION_TIMINGS=1` adds bounded child ranges for guide resolve, hierarchy,
screen trace, optional hardware trace and filtering/probes. It preserves dispatch/barrier order and is
off by default. Disable image dumps when collecting timings and check truncation; an outer inclusive
reflection range contains these child costs. Full-resolution SSR and optional hardware reflections must
be reported separately rather than treated as one default Forward+ workload.

`REMIX_DEBUG_FORWARD_FRAGMENT_STATS=<CSV path>` enables private fragment-invocation and precise
passed-sample queries. The runtime enables the supported Vulkan query features; other diagnostic hosts
must enable `pipelineStatisticsQuery` and `occlusionQueryPrecise` on their logical device too. Unsupported
devices skip capture. Queries preserve consecutive draw order, use 256 ranges per submission slot and
read completed slots without waiting. Check `available` and `omitted_ranges` before interpreting rows.
The CSV also appends `input_primitives` and `vertex_invocations` from the same pipeline query range.
These distinguish input geometry and actual vertex work from fragment invocations; do not infer
vertex cost from the fragment counter alone. Query-enabled runs are workload diagnostics, not an
uninstrumented performance baseline.
Categories: 0 probe/unmapped, 1 fully mapped, 2 partially mapped, 3 depth prepass, 4 terrain, 5 live GI.
The CSV includes extent, MSAA count and bake hash; zero hash is not a completed bake. Target recreation
appends another header and restarts frame numbering. The last pending slots are deliberately not read
at teardown. Fragment invocations and passed MSAA samples have different units; helper invocations,
discard and sample coverage prevent treating their ratio as exact pixel overdraw. Query-enabled timing
is instrumented timing, not a performance baseline.

The renderer already has an opaque depth prepass and tiled, depth-sliced light lists. Profile their
actual contribution before proposing another tiled-light path. The opaque-world timestamp includes
the depth prepass: subtract its child range when estimating colour shading alone. Compare completed
bakes with matched captured inputs, effective settings and camera views; broad FPS reports may also
include bake work, weapons, different resolutions or missing assets. An isolated highlight-budget
change must preserve bake hashes and distinguish specular selection from mixed-shadow selection.
GPU pass costs can shift between those ranges; inspect whole-frame time as well as child timestamps.
Bottom-of-pipe timestamps do not serialize subsequent commands. Neighbouring pipeline stages may
overlap, so a child-range increase does not by itself establish extra work in that pass. See the
[Vulkan timestamp synchronization scopes](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html).
Use `localShadowDraws` in the requested frame report to compare actual selected emitter identities
and per-face submissions. Those counts require no query instrumentation or GPU readback.
Keep the reported slow Crossfire gameplay case open until it is reproduced under its actual conditions.
The adapter's optional frame CSV now appends host/process foreground observations and native
window focus, minimized and drawable state. Exclude unfocused intervals from FPS acceptance;
failed window-state queries are unknown, not focused. Forced termination can truncate the final
record. Completed-bake and weapon/prop stability can still be inspected in an unfocused session,
but those checks do not establish foreground gameplay performance.
Use the roadmap's [map acceptance matrix](forward-plus-roadmap.md#map-acceptance-matrix), including
`gm_fork`, `d1_trainstation_02`, `bm_c1a1b` and installed large combined Half-Life addon maps. These additions
are pending coverage. Inventory addon BSP names and verify actual loaded content before recording
cold/warm, fixed-view and armed-traversal results; do not substitute repeated small-map checks.
`bm_c1a1b` also has user-reported authored parallax cubemap bounds to investigate. Verify their
encoding and route a future importer through the Source adapter into `SetReflectionVolumesEXT`;
capture seeds are not projection volumes. Import and performance acceptance remain pending.
For host profiling, unwind caller stacks rather than classifying a syscall from its address alone.
Observed main-thread sleeps lead through the original Source shader backend; the native worker often
waits on its condition variable. `mat_forcehardwaresync` was enabled in the sampled game. The initial
unfocused interval is excluded. A focused startup-only comparison changes Source/Vulkan overlap:
one view improves while another regresses, so keep the default unchanged. Effective-setting receipts
reject the unsuccessful Lua toggle trials. The sampled original shader-backend stack follows a
one-millisecond sleep in a query polling loop. Next measure event-query polling separately from
occlusion; preserve real query completion and readback lifetimes. Sampling overhead is excluded
from ordinary timing windows.
Adapter `OPENREMIX_PROXY_QUERY_PROFILE=1` adds per-kind interval counters to its slots report.
Query methods dispatch through the existing per-vtable resource registry, including event tables
in diagnostic mode. `GetData` timing excludes caller sleeps and query lifetime; rolling intervals
can straddle camera changes. Instrumentation adds overhead and is not an ordinary timing baseline.
Profiled calls keep original arguments/results. Bounded event retry with two poll caps was tested
and rejected: the slow view rarely completes during retry and shows no gain; opposite launch order
reverses the other view's apparent improvement. Retry code and tests were removed, leaving the query
profiler. Preserve FLUSH progress and device-loss returns in any future policy; never fabricate
completion or retry occlusion indiscriminately. Resume actual armed/moving gameplay and shading gates.
Clusters now retain separate runtime-prefix and complete-fallback counts in one ordered list. Overflow
is checked for the receiver's chosen count; unknown coverage keeps complete analytic fallback. GPU
membership tests include empty prefixes, independent tail overflow, late indices and either depth sign.
Influence bounds are prepared in view space once per light; shading still uses the original world-space
data. Private `.lights.bin` records are now 144 bytes (`clusterBounds` at byte 128), and the high bit of
`FrameData.clusterInfo.y` selects clustered baked-tail construction. Mask that bit before interpreting
the encoded prefix count. This changes private runtime buffers, not bake-cache payloads or the public ABI.

Ordinary-vertex draws specialize `UnmappedReceiver`: their vertex shader always emits invalid surface
chart coordinates, so those pipelines can discard atlas reconstruction code. Fully mapped and partial
chart layouts retain their existing specializations and probe fallback. This uses the same modules
and draw order; it does not approximate lighting or change cache identity.

The adaptive candidate improves five of six rendered receiver comparisons, with one worse. Its
original roughly 40% GPU regression is historical: ordinary receiver specialization and conservative
per-record light-volume support now make the current candidate faster than the installed uniform
build in the fixed Construct replay camera. That is not live-game or broad quality acceptance. The
hall's visible coverage is overwhelmingly charted, and the six test receivers are covered; missing
visible coverage did not explain that camera's cost. Fragment counters confirm the depth prepass
substantially reduces opaque colour shading work. Do not revive rejected shader experiments solely
because an old checkpoint had a different bottleneck.

Per-record probe volume bounds avoid wasting coarse reach on fine-only records. In Construct replay
this restores eligible light-volume coverage and permits the white-room controllable group to publish.
Decline diagnostics distinguish missing owners, absent pairs and unreceived pairs; unknown coverage
still falls back. c1a0, Crossfire, c1a0d, Lost Coast and Steamlab publish and reload matching fixed-view
caches. c1a0 required the empty-coarse-seed fallback. Static test content must mount shared Source
assets before diagnosing unresolved materials as bake failures. The adapter's optional
`OPENREMIX_PROXY_CONTENT_MOUNTS` lists absolute game-content roots without copying original archives.
The original c1a0d locker-room noise, other-map quality and live performance reports remain open.

A saved **zero-volume authored reflection set disables automatic local cubemaps** by API contract.
It is different from having no override. Check the active count and authored status in F12 or
`probes.json` before diagnosing room fitting or profiling the fallback. The adapter's sidecar lives
under the game's `garrysmod/data/openremix/cubemaps` directory unless
`OPENREMIX_PROXY_CUBEMAP_BOUNDS` overrides its path. Removing a sidecar restores automatic placement;
an invalid edit preserves the previous valid override. Do not delete authored data merely because it
is empty: use an isolated nonexistent override path for a diagnostic session. The test install was
found to contain an empty Construct sidecar. On 2026-10-22 that override was renamed to
`gm_construct.cubemaps.bin.disabled-empty-20261022`, preserving its contents while restoring
automatic placement on the next map load. Live visual confirmation remains pending.
The user subsequently authored two Construct volumes, restoring a nonempty active override.
Their JSON is preserved in the adapter's `tools/probe_editor/references/gm_construct.json` as
a partial main-hall fitting reference. Preserve the live authoring document and runtime sidecar;
these two volumes do not establish whole-map reflection coverage. The optional setting
`rtx.native.forward.authoredReflectionFallback=1` now fills uncovered areas with existing automatic
image layers within the shared 64-record budget. It defaults off, preserves explicit-empty/null-reset
semantics, reports covered/omitted counts, and does not invalidate diffuse lighting. CPU/GPU copy and
production edit tests pass; map-scale quality and memory/performance acceptance remain open.

Keep GPU frame, transport and startup timing separate. Live profiles must verify the screenshot,
active bake identity, effective reflection options and resolution before interpreting frame costs.
The default SSR path and optional hardware reflections have very different costs.

Rejected and removed: generic fragment offline optimization, fully fine-weighted coarse-sampling shortcut,
separate mapped fragment modules, reordered category profiling, homogeneous draw-range splitting,
compact cell-address stencil indices, unified mapped/partial pipelines, and explicit products replacing
fixed-power stencil weights. Hoisting styled-probe accumulation outside the corner loop was also slower
and was restored; do not revive it without new evidence. These preserved output in
many controls but did not establish a performance gain; reordered profiling changed coplanar pixels.
Some paired runs drifted despite stable clocks: do not treat small percentages as precise hardware laws.
Driver local-memory statistics were implausible; do not silently mask them into invented spill counts.

Viewmodel cubemap candidates are prepared by `forward/probes/receiver_mask.*` from posed draw bounds,
including the normal-query padding. Uncertain inputs retain candidates. The private viewmodel screen
uniform is 112 bytes and appends two candidate words; rebuild CPU and embedded shader together. Public
ABI, probe headers and bake policy are unchanged. The diagnostic unculled-probe environment option
must be set before process startup. Exact 64th-probe frame parity and fixed live costs support this path;
other weapons/maps and moving receiver quality remain acceptance work.

## Adapter and authoring

The adapter owns BSP/Source parsing, range/unit conventions, style identities and cubemap sidecars.
Native lighting remains engine agnostic. Bake material readiness uses cached dependencies of
retained geometry; unused inventory and engine-owned sky do not block it, but missing required
materials do. The supported static-prop `Refract` import now resolves Trainstation's
`models/props_combine/com_shield001a`, including normal/tint maps and animation proxies.
Native measured thin-sheet bake transport is implemented and tested, including textured absorption,
both interface directions and containing-volume classification through panes. Native live sheets
now use the late MSAA refraction pass, with textured absorption, an exit-point offset, visible-sky
transmission and shared reflections. The primary sheet assumes external air; camera-media/nested
integration and full path-tracer visual parity remain pending. Vulkan primary and
continuation paths now share the measured optics model; raw ray/raster, stacked-sheet and actual
SHARC-query transport checks cover sheets in air. Textured primary optics, roughness and denoised
motion/lighting guides still need acceptance. These native paths are available in the testing
checkpoint. The newer paired candidate unblocks Trainstation through explicit stationary
mean optical tint, including tint that shares animated bump UVs. Source's artistic refraction amount is not a physical IOR. Do not bypass the
required material.
Vulkan path-tracer light visibility now uses optional descriptor-indexed sheet optics with the
shared two-interface transport model. Its surface/frame GPU layouts are unchanged. Primary and
continuation shading use the measured model; Metal sheet rendering remains legacy. The shadow visibility fixture
explicitly enables blended secondary rays; the renderer's existing skip policy is unchanged.
The installed combined addon supplies chapter BSPs including `hls00amrl` and `hls05amrl`; exact
`gm_fork` availability remains pending, and the installed RP Fork variant is not a substitute. `bsp-replay` supports repeatable default-off
`replay.probeReceiver = X Y Z radius` matte dynamic spheres for receiver comparisons.
See the adapter's `tools/probe_editor/README.md` for the existing native Qt cubemap workbench.
Authored references align with **hls00amrl**, not hls01amrl. Use combined cubemap/probe volumes and their
independent capture origins, not ordinary point entities alone. New standalone tools should prefer C#.

Refract reference inspection of the local s&box Source1 importer/shader confirms normal-frame
selection by `$bumpframe` and tint sampling through transformed normal UVs. The shield proxy
names `$dudvmap` but writes `$bumpframe`; matching only the animated texture name would miss
its normal playback. Native sheet tint now supports explicit scrolling UV rates, which
the adapter binds alongside normal scroll. Keep the artistic refraction amount separate from IOR,
define passive optical tint conversion explicitly, and preserve replacement keys.
Adapter `4297c7a` fixes a prerequisite: water normal cache/upload identity now includes the
playback rate, so shared texture names with different rates cannot alias the same mutable
runtime texture. Refract integration now shares these surface-normal resources; broader
live multi-rate validation remains pending. This change is now in the testing install.

Native optical tint scrolling is now implemented as an optional engine-agnostic measured-sheet
control, with separate UV rates and a streamed clock. Forward+ shares one animation time
between live surfaces and ray transport; exposure adaptation keeps its independent input.
Ray-scene identity depends on rates, not ticking time. Vulkan path-tracer primary/continuation
and light visibility share the tint UV helper; procedural material flags also include tint.
Frozen tint now supports an explicit per-axis uniform-UV distribution through
`MaterialBakeTintAverageEXT`. Interactions independently sample optical response, preserving
unselected spatial axes; this is not a coherent global animation-time simulation. All moving
axes must be covered, and mutable tint images remain rejected. Playback rate changes preserve
the averaged bake hash; live rays ignore the policy and keep actual rates. The adapter now
creates supported Refract sheets with passive tint conversion and proxy bindings, unblocking
Trainstation. Metal parity and broader visual qualification remain open.
This source change is now in the testing install.
Averaging checks cover invalid/wrong/missing axis policies, immutable-image requirements,
snapshot rate retention and stationary hash identity. GPU horizontal/vertical tint cases
check the mean optical response, preserved unselected detail and clock-independent results.
Live capture checks ensure bake-only flags neither alter live sampling nor force rebuilds.
Affected source/binary checks pass; this component is now in the testing install.
CPU validation, frozen-bake refusal/restored hash, stable live acceleration keys, descriptor
and procedural-clock ownership checks pass. GPU transport and Forward+ MSAA surface tests
exercise tint shifting/wrapping; exposure, path-tracer primary-sheet and light-visibility
regressions pass. Affected shader source/binary checks pass. Nonprogressive path-tracer
animated image acceptance and Source adapter integration remain open.

A 2026-10-24 installed-build recheck confirms this is still a pre-bake publication blocker:
614/615 resolved materials, retained geometry present, active bake hash zero. No GPU/FPS
timings from that attempt are accepted. Temporary HL2 mounts and protected settings were
restored after ending the owned run. Adapter diagnostics now distinguish unsupported texture
sources from uploads still loading; this does not implement Refract or bypass readiness.

## Release/handoff gates

1. Review candidate policy and CPU/GPU/cache contracts; preserve existing default performance.
2. Reproduce regressions on fixed cameras, then validate moving receivers and other maps.
3. Finish documentation and dependency/package review; optional OIDN/backend support needs distribution checks.
4. Merge future upstream changes deliberately and rerun affected checks; do not alter main or user configs.
5. Deploy only an explicitly reviewed package to the separate test install; preserve addon mounts, config,
   gameinfo and runtime settings. The adaptive default is not release-ready merely because unit tests pass.

The local handoff bundle supplies machine-specific paths, binary/cache hashes, evidence receipts and
reproduction configurations. Those verification artifacts intentionally stay out of tracked feature docs.

## Offline capture preparation readiness

The source API adds independent `remixapi_GetPreparationStatisticsEXT`; it leaves existing backend
and interface layouts unchanged. The replay requires it for measured and camera-path captures,
waiting for loading/preparation to clear and requested/completed pipeline counts to agree. Compilation
failures or preparation restarting after warmup reject the run; pending waits are bounded. Manual
fixed-frame use with older runtimes warns that readiness cannot be verified. Final readiness is saved
in `.preparation.json`. The installed `ffc32fe` provides this export; the older `22b8406` did not.
Rebuild older isolated shader experiments before using that tool.
Focused API and CPU readiness checks pass. The original prepared path-traced check emitted a
timeline warning and is excluded from clean reference/performance acceptance; the isolation below
provides the current validation scope.

The timeline warning was subsequently isolated with the installed 1.3.280 validation layer: setting
only `rtx.native.lowLatency=0` for the offline reference yields a clean capture at 35/35 pipelines.
Runtime code uses the specification's driver-signalled Reflex timeline pattern; current validation
source tracks that external signal while the old version lacks the hook. Keep graphics synchronization
and game Reflex defaults unchanged. Explicitly record Reflex-off when validating offline references
with that layer; this does not settle the older retained-process problem or establish FPS.


## Cubemap experiment status

Dense-grid Construct sphere comparisons now separate the sparse-grid sunlight leak from reflection
quality. Dominant-direction lookup and numerical GGX energy correction both have roughness-dependent
tradeoffs; neither is promoted. Production shaders/defaults and the installed package are unchanged.
Use the current roadmap section for the decision and remaining controls. Before changing reflection
shading, isolate capture resolution/origin, parallax and transport convergence; then evaluate material
energy and receiver-lighting normalization. Single-camera metallic-sphere results are not broader
map, water, viewmodel, motion or FPS acceptance.


Normal-quality transport is required for the reflection study: three bounces restore ceiling
illumination missing at one bounce, reducing the controlled mirror-region error by roughly an order
of magnitude. Runtime defaults already use 256 samples and three bounces. Earlier cheap-transport
comparisons cannot decide a production direction/energy change. The adapter's
`tools/replay_quality.cfg` provides an explicit starting point; add fixed camera, matching assets,
exposure and sky scope, and still verify preparation and transport convergence.


HDR sky replay requires the native immutable float texture formats included in the installed
`ffc32fe` refresh. The earlier `22b8406` runtime predates them. Pair rebuilt replay tools with a
compatible native runtime. LDR/frozen-reference behavior remains compatible; see the adapter README
for encodings and limits.


Late-emissive temporal layout changes now use the runtime's compiler-worker preparation contract.
Scene source eligibility is sticky independently of the currently prepared layout, so spawning an
emissive object requests a replacement while the previous renderer remains usable. Hiding/showing
it does not repeatedly replace temporal targets. Existing Vulkan error handling, queue fences and
public ABI layouts remain unchanged. The added GPU regression covers the late-source transition,
TAA retention and warmed visibility variants; the original map failure now captures cleanly. The
separate validation-layer cleanup diagnostics in rejected negative controls remain unresolved.


### Refract importer integration checkpoint

The adapter now imports supported Source Refract materials as measured finite
sheets, preserving normal/tint scroll and original texture replacement identities.
The passive linear tint conversion clamps Source texture amplification; this is
an explicit physical approximation, not Source screen-space refraction parity.
Animated tint images and mismatched normal/proxy frame counts fail with diagnostics.
Static baking uses smooth interfaces and independent stationary tint UV phases;
live rendering keeps the scrolling material. Shared surface-normal resources keep
ownership and playback-rate identity, independent of the water debug switch.
Repeated Source asset separators now normalize consistently for loose/VPK lookup.

This supersedes the earlier Trainstation missing-material blocker notes: complete
relay static inputs and first bake/cache reuse are now working with the paired
native/adapter candidate. The installed test game now uses the paired package described above.
Targeted animated-sheet visual validation, pathtracer parity, moving-camera cache
stability and foreground in-game performance remain open qualification work after the testing refresh.
Do not treat relay completion or its small render target as game FPS acceptance.

Controlled Refract comparison note: the earlier pathtracer close-up retained
automatic exposure and bloom while Forward+ used manual exposure. A repeat
with both at manual exposure 1 brings scene brightness much closer; do not
attribute that earlier difference to bake transport. Specular appearance and
animation still need targeted qualification. The replay quality template now
pins matching Prism/gamma/manual exposure settings for both backends.

Offline transport now submits independent required compute kernels as one
preparation batch through the existing bounded pipeline service. Optional
variants retain their existing policy gates; shader modules, pipeline layouts
and dispatch semantics are unchanged. This removes serial create/wait
scheduling during cold compilation. Transport and cancellation validation,
plus repeated Construct cold-start/image comparisons, pass. The testing
installation now includes this candidate.

Current runtime profiling priority: expensive generic receiver/world and
viewmodel shading in light-heavy views, preserving unknown probe/lightmap
coverage fallback. The clustered baked-light tail is correctness work, not
proof of analytic diffuse evaluation. Separate host-limited views from
GPU-limited views before selecting optimizations. Static shadow-map update
counters remain zero after bake in the qualified Trainstation fixed views.

The viewmodel-only probe-gather selector was tested and removed: repeated
focused Trainstation comparisons did not improve whole-frame GPU cost, despite
a local viewmodel improvement in one view. World consumer specialization stayed
on the baseline, so the observed cost increase is not proof of shader-code
causality. Keep the ordinary path and the existing broader opt-in experiment;
do not restore the narrower selector without new supporting evidence.

Construct review now includes entity-located secret-room cameras near
(-3040, -1248, -32), identified by `soundscape_secret` and nearby furniture
entities and verified nonsolid in the BSP tree. The old (2104, -800, -90)
camera is corridor follow-through, not confirmed secret-room coverage. Paired
neutral irradiance and ordinary shading at five installed-build viewpoints
retain matching bake data and do not reproduce the old coloured edge fringe
or fixture halo. This is static relay coverage, not interactive acceptance.
The glossy connector ceiling response needs specular/SSR/cubemap isolation and
a matched-material reference before changing lightmap packing or quality.

Connector isolation retains the fine ceiling highlight with SSR disabled and
in the direct-specular diagnostic (which excludes environment composition).
Explicitly disabling selected baked highlights removes that fine shine while
leaving the broader baked lighting intact. Keep the default highlights: check
the source/AutoPBR normal and roughness response against a matched reference,
then visibility if necessary, before labeling this an occlusion or atlas bug.

The connector ceiling is the map-patched `concretefloor028a` at Z=-8. Its
replacement roughness is exactly the inverse of Source normal alpha, including
near-zero patches; it is not a uniform fallback roughness. Keep the material
unchanged pending broader material review. The earlier dark pathtraced
reference used the relay's erroneous 16-Source-unit light cutoff. Replay now
shares Source lighting defaults with the live adapter; the corrected reference
is lit and retains the ceiling highlight. A cutoff-only control reproduces
the dark image. This resolves that reference mismatch, not every parity gate.
Accumulation references must explicitly disable TAA; the relay now reports
active/requested temporal states instead of hiding such readiness conflicts.


### 26 October 2026 - correction: movable func_brush stays probe-lit

The user clarified that func_brush is movable and must remain probe-lit. The
adapter experiment was reverted in full, including its classification, charts,
reference lifetime module and flag. vrad_brush_cast_shadows=0 is an occlusion
setting, not proof of immobility; authored-pose matching and movement fallback do
not justify allocating a lightmap for an entity intended to move. It was never
enabled in the playable package. The experimental white-room speedup is rejected
as the implementation direction, not a candidate for default promotion.

Supersedes earlier tasks proposing a controllable func_brush receiver relay.
Keep stationary world surfaces and established static models in lightmaps;
movable brushes, props and viewmodels use probes. Optimize the probe consumer,
coverage/visibility lookup and baked-light evaluation without changing that
ownership policy. Retain generic receiver-only native APIs only for hosts with
an independent, authoritative stationary-surface contract; they do not authorize
func_brush classification.

Next: isolate the white-room probe cost while preserving materials/specular,
styled-light response and unknown-coverage fallback. An unlit material can still
have specular response, so skipping all lighting is not a valid shortcut.


### 26 October 2026 - rejected runtime zero-diffuse probe shortcut

A combined runtime shader trial skipped diffuse SH and directional diffuse
correction gathers for exactly zero diffuse materials, preserving probe ownership
and scalar/specular visibility. Movable func_brush stayed on probes throughout.
Normal-quality foreground Construct before/candidate/restored samples show room
GPU 9.86-9.99 -> 9.17 ms, but hall 2.62-2.71 -> 3.01-3.05 ms. Room image comparison
is identical; this is narrow evidence, not broad quality acceptance. The combined
trial does not isolate the two gathers, and changing depth timing prevents a
simple instruction-only interpretation. Candidate shader edits were removed and
the ordinary renderer rebuilt; no flags or unused pipeline paths remain.

Next investigate separately selected zero-diffuse material variants while keeping
the ordinary shader unchanged for other materials. Conservative material/texture/
blend eligibility and GPU readback for specular, mixed shadows, covered/uncovered
probes, styled lights and metallic/emissive materials are required. Register or
occupancy effects are hypotheses until measured. No default or deployment change.


### 26 October 2026 - separate zero-diffuse probe shader candidate

The replacement for the rejected runtime predicate uses separate fragment blobs
and an opt-in world pipeline (`REMIX_DEBUG_ZERO_DIFFUSE_MATERIAL=1`). All 22 old
forward shader blobs compare byte-identical. A conservative material helper proves
zero diffuse for constant full metal without a metallic texture, black final RGB
tint, or constant full self illumination without an emission texture. Textured,
masked and partial cases keep ordinary shading unless another independent proof
holds. Movable brushes remain probe-lit; no brush bake classification was added.

Normal-quality same-binary enabled/disabled/enabled foreground Construct runs:
white-room GPU 9.74-9.77 ms control versus 8.42-8.52 ms enabled, about 13% lower.
Hall repeated timing is approximately 2.6 ms with either path. All three sessions
retain identical bake and content hashes; the room image comparison is identical.
This avoids the earlier broad hallway regression but is not all-map acceptance.

CPU eligibility checks and selected-light GPU tests pass with the variant enabled
and disabled. New black-dielectric and full-metal dynamic probe checks retain
selected specular and moving shadows at 1x/4x MSAA without rebaking; their sampled
pixels match both paths. Ordinary, mapped, terrain, water, live-GI and viewmodel
paths retain their existing shader selection. No default or playable deployment
change. Continue Crossfire/c1a0, material edits/animation, unknown coverage and
styled-light qualification before promotion. Local probe visibility remains a
major remaining cost; this only removes unused diffuse gathers.


### 26 October 2026 - map smoke checks and scoped batching follow-up

The zero-diffuse material candidate has matched normal-quality Crossfire runs
with no obvious GPU regression (about 1.9-2.0 ms yard, 2.7 ms indoors). All four
comparison rectangles are identical and mode pairs share bake/content hashes.
Private test mounting and weapon-pickup issues were corrected before accepting
samples; failed/unresolved/held-weapon attempts are excluded.

c1a0 reception/wall runs are functional smoke evidence, not isolated performance
qualification. GPU is about 2.55-2.60/1.58-1.59 ms, but active map NPCs have different
poses/draw counts between cold and warm sessions. The wall images are identical;
reception differs around animated NPCs. Match map/NPC age and repeat before
attributing small timing differences to the shader or promoting its defaults.

The GPU fixture now returns black/metal probe receivers to ordinary white diffuse
materials and requires exact original pixels with the same bake volume. Both
ordinary and specialized tests pass with selected-probe batching also enabled,
including Vulkan core/synchronization validation.

One exploratory combined material-variant plus broad-probe-batch Construct run
shows white-room GPU 6.26-6.35 ms, versus previous material-only 8.42-8.52 and
original 9.74-9.77 ms. Bake/content hashes and room image rectangle match. Field
residency is about 2 MiB (about 4 MiB peak) and reuses exceed 5000. This is not a
fresh interleaved batching comparison. General batching has earlier tradeoffs.
Next investigate material-scoped visibility batching with unchanged ordinary
shader specialization, then repeat controls and unknown-coverage/material editing
gates. Keep both experiments opt-in; no playable/default change has been made.

Cold Crossfire adaptive probe placement still takes about 11.26 s. Denoising and
styled reconstruction remain preparation costs to improve independently of frame
shading. Continue the viewmodel/world coverage work with the SSR contract intact.


### 2026-10-26 - Playable deployment refreshed

Playable GarrysModWithRTX2 and local updater now use immutable package
forwardplus-probe-planning-20261026 (native daf54c57, adapter6495e0c). Verified157
owned files,101protected files unchanged. Includes receiver-clearance reuse and
probe allocation diagnostics; experimental lighting flags remain off by default.
Installed normal-quality Construct fixed-view replay passes with exact historical
image parity and clean Vulkan validation. Path-tracer primary sheet/energy GPU
regression passes; no installed PT RR or gameplay FPS qualification claimed.

- [x] Include confirmed clearance reuse in a validated playable package.
- [ ] Continue Construct probe placement/visibility and seam/noise quality work.
- [ ] Study tiny-chart denoising costs with quality-preserving comparisons.


### 2026-10-26 - Private coarse probe transfer reconstruction

Optional immutable coarse-to-fine visibility fields now support the actual CPU
probe stencil, with root/fine addressing and physical donor identity checks.
Unknown/missing connections preserve fallback; malformed fields leave weights
unchanged. Private CPU diagnostic copies only: production fragment sampling and
baked components remain unchanged. No new playable deployment.

- [x] CPU reconstruction energy, continuity, unknown-data and donor identity tests.
- [x] Normal-quality Construct diagnostic agrees with offline ceiling prediction.
- [ ] Add fragment publication/sampling and verify CPU/GPU parity before enabling.
- [ ] Qualify biased queries, transitions, styles, moving props/weapons and cache upgrades.
- [ ] Continue surface noise/seams, tiny-chart denoising and frame-cost work.


### 2026-10-26 - Private GPU coarse probe transfer

Production shared probe stencil can now sample the prepared immutable visibility
field under REMIX_DEBUG_COARSE_TRANSFER_SAMPLE=1. Field shares the volume buffer;
GPU publication checks combined device storage range. Header bit30 marks transfer,
bits1..29 retain local-table offset; styled/depth tables remain independently
addressed. Disk cache and ordinary defaults unchanged.

- [x] GPU publication and production decoder, including styled/depth coexistence.
- [x] All-axis CPU/GPU reconstruction parity, shared boundaries and changed blends.
- [x] Private normal-quality Construct candidate/control; clean core/sync validation.
- [x] Flag-off cached replay retains exact historical fixed-view image parity.
- [ ] Main-room props remain too dark; tested prop rectangles unchanged by correction.
- [ ] Diagnose actual prop stencils/material response versus independent reference.
- [ ] Qualify moving props/weapons, styles, invalidation, cache upgrade and frame cost.
- [ ] Continue surface seam/noise, density allocation and tiny-chart OIDN work.

No playable deployment or general performance/lighting-quality acceptance yet.


### 2026-10-26 - Construct prop reference study

Added published rendering.debugView diagnostic identity to prevent captures of
pending shader views. Explicit hemisphere/surface reference requests now emit
CPU base-grid stencils and irradiance beside integration results. Read-only,
bounded diagnostic work; no normal shading or public ABI changes.

- [x] Confirm actual neutral/coverage/material views after shader publication.
- [x] Six approximate prop sites compared to8192direction hemisphere reference.
- [x] Distinguish fine-only prop errors from coarse fallback ceiling leakage.
- [x] Test density and probe sampling independently; surface quality unchanged.
- [ ] Separate SH angular truncation from spatial interpolation at query/donor sites.
- [ ] Validate exact prop normals, motion, other rooms/maps and quality-cost policy.

Baseline32763probes gives median19.7% luminance error at six sites;121279probes
still19.5%; same placement with2048probe samples14.1%, with40.8% residual side
underestimate. Errors include overbright faces. No global brightness/density fix
or default change justified. Reference shares transport and has unquantified
Monte Carlo error; not full Source2 or physical-reference acceptance. Surface/
sun/capture hashes match baseline when only probe samples change. Continue
surface quality, frame cost and bake-time work; playable remains unchanged.


### 2026-10-26 - Angular versus spatial probe reconstruction

Added bounded sphere reference mode: incoming radiance projected to cosine-
convolved SH on CPU, with no analytic receiver term or probe interpolation.
Analytic constant/linear six-axis, moment, batch and readback tests pass. Private
Construct12site run has clean Vulkan validation and restored host settings.

- [x] Match incoming-radiance terms for SH versus hemisphere comparison.
- [x] Six same-position SH controls differ by1.1-7.7% luminance; biased positions
  differ by0.1-6.7% versus original hemisphere reference. Shared transport and
  unquantified Monte Carlo/quadrature uncertainty; not physical-renderer proof.
- [x] Offline weight ablations reject simple trilinear/facing/visibility removal
  as a consistent fix; no production interpolation change.
- [ ] Compare actual donor-origin high-sample fields/variance against receiver sites.
- [ ] Evaluate selective receiver-driven placement and interpolation with leakage,
  style, motion, map-variety and frame/bake-cost controls.

Angular approximation and bias alone do not explain the larger tested prop-field
errors. No default/playable change; continue surface quality and performance work.


### 2026-10-26 - Matched lighting terms and finer probe placement

Earlier hemisphere/sphere percentage comparisons omit analytic receiver direct;
they are incoming-only differences, not complete physical probe errors. Added
explicit analytic-lighting policy metadata to reference and CPU grid diagnostics
so matching-term comparisons can be checked. Native build and reference tests pass.

- [x] Complete8192sample query/donor SH and surface reference comparisons.
- [x] Direct donor-face integrations isolate angular versus spatial effects.
- [x] Matched full-lighting reference: baseline median21.9% luminance error;
  2048sample-only12.6%; replacing donor captures still leaves barrel side42% low.
- [x] Combined finer-placement/high-sample131k-budget experiment yields median6.6%,
  maximum15.2% at six sites; barrel-side error.7%. Actual query patch2m, not1m.
- [ ] Equal-sample/lattice budget controls, smaller-budget placement prioritization.
- [ ] Real frame/bake/memory costs, styles, motion, leakage and broader map quality.

VRF viewer uses six-face ambient-cube volumes; accurate axial donor values at the
same locations still leave the spatial error. Representation change alone is not
supported as this case's fix. No default or playable change; surface quality and
performance work remain active. Uncertainty and shared-transport limits remain.


### 2026-10-26 - Probe budget controls and exact donor diagnostics

At2048probe samples and unchanged surface quality,65536budget/requested1m retains
all six prior131072 site values exactly (query patch2m). Median/max matched luma
errors6.58%/15.18%; RGB vector errors7.67%/21.93%. Blue remains28% low on one face.
65536/requested2m gives13.17% median luma error despite nominal2m at those queries;
root lattice/relocation/capture controls matter.32768/requested1m gives mixed gains
(median11.19%, maximum26.15% versus sample-only12.62%/41.45%); not a blanket fix.

- [x] Halve promising quality budget with six-site equality, not whole-map parity.
- [x] Compare original-spacing and original-budget policies at equal sample count.
- [x] Include RGB error and actual physical record costs; no FPS/bake-time claims.
- [x] Add exact retained trace origin, spacing and visibility statistics to reference
  grid report; null for unavailable CPU data. Native build and live origins verified.
- [ ] Check exact relocation/terrain geometry validity and refinement priorities.
- [ ] Qualify65536/1m/2048 across other rooms/maps, motion/styles/leakage and costs.
- [ ] Improve selective placement within smaller budgets before default promotion.

No normal rendering/default/playable changes. Physical probe payload16MiB at64k
excludes auxiliary/staging storage. Ordinary selected-light gather inactive here;
qualify experimental residency separately. Continue surface and performance roadmap.


### 2026-10-26 - Geometry-blocked fine donors; depth filter trial rejected

Exact donor-to-query CPU rigid opaque-segment checks find horizontal geometry
before eight of sixteen connections. Blocked normalized contribution14.8% at
barrel front and.65% side in original-budget placement. CPU check ignores alpha/
transmission; finite HWRT material-coverage confirmation remains required.

- [x] Verify actual trace origins/biased endpoints against complete retained geometry.
- [x] Existing private directional-depth mode does not reliably suppress these
  links (18.8%/7.1% known blocked shares versus14.8%/.65% ordinary).
- [x] Per-tap visibility-filter prototype passes synthetic/CPU/GPU tests but fails
  map controls (15.3%/9.1% blocked shares, worse lighting); candidate removed.
- [x] Original source/native restored; cached image parity and clean validation;
  playable files unchanged. No rejected path/flag retained.
- [ ] Confirm finite HWRT coverage, inspect tap mixing, and evaluate geometry-aware
  fine donor selection/relocation without creating missing-lighting holes.

Directional-depth remains private/off. Keep64k quality candidate, wider map/room,
styles/motion/color, runtime/bake/memory costs, surface quality and coarse-transfer
lifecycle/cache gates active. No default/playable change.


### 2026-10-26 - HWRT confirms donor blockers; angular resolution control

Added bounded geometry reference input using existing fenced geometry transport:
max16 unit rays/4KiB, explicit hit/miss/range/backface metadata, no fake SH values.
No GPU shader/public API/default change. CPU tests and native build pass.

- [x] All16 finite donor connections agree with CPU:8blocked/8clear, first-hit
  distances within.000053map units after material coverage. Not inside-solid proof.
- [x]8x8 tap captures show near/far mixing (60.7..1924.7units in one footprint).
- [x] One bad donor's16x16 footprint reduces reconstructed visibility.1527->.0000153;
  bounded offline field-decoder reproduction, not complete map acceptance.
- [x] Short owned harness stops after geometry receipt; four clean validation runs;
  cached normal render unchanged and protected playable files preserved.
- [ ] Prototype16x16 field with output-stride/device/readback/cache/lifetime tests.
- [ ] Qualify all blocked donors, geometry, temporal/color/style coverage and
  storage/trace/frame cost across rooms/maps before enabling by default.

Existing8x8 remains private/off; rejected per-tap change remains removed. Continue
64k quality-policy, placement, surface noise/seams, denoising and frame/bake roadmap.


### 2026-10-26 - Reject blanket 16x16 probe visibility upgrade

- [x] Private prototype expands directional geometry fields to 16x16, including
  four-result output stride, fenced readback, GPU addressing and cache identity.
- [x] CPU/cache/GPU reconstruction and independent HWRT multi-probe checks pass.
- [x] Construct controls confirm much lower cross-floor donor contribution, but
  worse total lighting error. Candidate removed; no default/playable change.
- [ ] Improve geometry-aware local probe support/placement and interpolation;
  validate both clear and blocked connections, color, styles and motion.
- [ ] Qualify 64k adaptive placement across rooms/maps and memory/bake/frame cost.
- [ ] Continue surface noise/seams/denoising, lightmap packing and runtime baked
  light-loop profiling independently. Representation or density alone is no fix.

Existing 8x8 directional field remains private/off; per-tap filtering remains
rejected. Reference viewer shaders do not reveal the native CS2 baker.


### 2026-10-26 - Separate probe sampling from density and visibility

- [x] Test 64k / requested 1m layout at 256, 1024, 2048 and 4096 probe samples.
  Planned positions/surface data match; six site stencils have identical donor
  IDs and effectively identical weights. Captured lighting still changes.
- [x] Low sample count retains a severe local outlier despite a good median;
  4096-sample control lowers worst-site error, without proving full convergence.
- [ ] Isolate dominant donor SH/capture convergence; evaluate variance-guided
  sampling and importance sampling before promoting a blanket quality preset.
- [ ] Broader spatial/color/style/motion/frame qualification for adaptive density.
- [ ] Optimize denoising field/chart scheduling while preserving directional
  data and seam isolation; measured denoising dominates probe trace time here.

No default/playable change. Median quality alone is insufficient acceptance.


### 2026-10-26 - Isolate incoming probe capture variance

- [x] Add bounded incoming-SH reference capture using existing irradiance
  transport with receiver-direct NEE disabled only for that dispatch.
- [x] Matched 256/8192-sample controls at the dominant Construct crate donor
  isolate a large incoming-light shortfall; receiver-direct mean is similar.
- [x] Parsing/range/output and HWRT transport tests cover the split and normal
  direct-light restoration on slot reuse. No shader/default/published layout change.
- [ ] Evaluate environment importance sampling with correct MIS, progressive
  path dimensions and variance-guided probe sampling. Current textured dome
  contributes on path misses; hit-point NEE currently samples sun/local lights.
- [ ] Retain spatial support/placement and SH representation controls; converged
  captures at a different position do not prove correct prop lighting.

No quality preset or playable update from this diagnostic alone. Surface
noise/seams, denoising cost and frame profiling remain independent priorities.


### 2026-10-26 - Reject reference-only sky-MIS prototype

- [x] Prototype textured-environment importance sampling and diffuse continuation
  MIS; constant-sky energy and HWRT validation pass. Equal-primary-sample
  Construct test adds shadow rays without a useful noise reduction.
- [x] Remove entire candidate, including options/flags/bindings/allocations/tests.
  Original implementation restored; no default or playable update.
- [x] Same-origin six-axis cosine controls separate SH angular approximation
  from low-sample noise. Independent low-budget face captures retain outliers.
- [ ] Compare positive ambient-cube and SH accumulation from identical primary
  radiance samples, reusing rays across faces; do not infer quality from one face.
- [ ] Evaluate progressive path dimensions and variance-guided probe allocation.
- [ ] Preserve spatial support, styles/motion/color/cost gates, surface denoising
  and runtime profiling priorities. HDR-map sky sampling remains unqualified.


### 2026-10-26 - Matched SH and ambient-cube capture

- [x] Reuse identical incoming rays for CPU SH and six positive ambient faces.
  Explicit sphere phases preserve origins, old zero-phase rays and batch stability.
- [x] Constant/linear projection and bounded request/output tests pass.
- [x] Construct matched controls show lower high-sample cube axis error but no
  general low-sample noise improvement. No renderer representation/default change.
- [ ] Test primary/path sampling and convergence at equal tracing work.
- [ ] Keep ambient-cube runtime/storage work separate from noise, spatial
  support, arbitrary-normal interpolation, visibility and styled-light gates.
