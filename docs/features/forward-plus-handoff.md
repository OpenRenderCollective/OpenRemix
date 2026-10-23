# Forward+ continuation guide

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
The source integration is qualified; deployment packaging and installed verification remain. Do not remove these instances from all transport. The installed native
`d9d54c2` / adapter `a499842` package remains unchanged.

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
Use `python scripts/shaders/generate.py <backend> --check` and separately `--validate`; these flags are
mutually exclusive. Affected backends: forward, live_gi, water, water_ray, probe_test, forward_test, style_test.

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
materials do. `d1_trainstation_02` remains blocked by its required static-prop `Refract` material
`models/props_combine/com_shield001a`, which has normal/tint maps, animation proxies and no base texture.
Native measured thin-sheet bake transport is implemented and tested, including textured absorption,
both interface directions and containing-volume classification through panes. Native live sheets
now use the late MSAA refraction pass, with textured absorption, an exit-point offset, visible-sky
transmission and shared reflections. The primary sheet assumes external air; camera-media/nested
integration, full path-tracer parity and adapter integration remain pending. Vulkan primary and
continuation paths now share the measured optics model; raw ray/raster, stacked-sheet and actual
SHARC-query transport checks cover sheets in air. Textured primary optics, roughness and denoised
motion/lighting guides still need acceptance. These native paths are available in the testing
checkpoint; Trainstation is still blocked. Define stable mean optical tint explicitly because tint can share
animated bump UVs. Source's artistic refraction amount is not a physical IOR. Do not bypass the
required material.
Vulkan path-tracer light visibility now uses optional descriptor-indexed sheet optics with the
shared two-interface transport model. Its surface/frame GPU layouts are unchanged. Primary and
continuation shading remain legacy, as does Metal sheet rendering. The shadow visibility fixture
explicitly enables blended secondary rays; the renderer's existing skip policy is unchanged.
The installed combined addon supplies chapter BSPs including `hls00amrl` and `hls05amrl`; exact
`gm_fork` availability remains pending, and the installed RP Fork variant is not a substitute. `bsp-replay` supports repeatable default-off
`replay.probeReceiver = X Y Z radius` matte dynamic spheres for receiver comparisons.
See the adapter's `tools/probe_editor/README.md` for the existing native Qt cubemap workbench.
Authored references align with **hls00amrl**, not hls01amrl. Use combined cubemap/probe volumes and their
independent capture origins, not ordinary point entities alone. New standalone tools should prefer C#.

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
