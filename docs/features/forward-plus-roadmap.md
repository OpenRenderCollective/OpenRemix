# Forward+ delivery roadmap

The intended result is a cheaper, Source-2-style rendering backend for GMod: forward PBR shading with
MSAA, hardware map-load surface lightmaps and probes, attributed baked direct lighting, soft static
shadows combined with dynamic-object shadows, automatic tolerant parallax cubemaps, rough SSR, and
optional hardware reflections/GI. Replacement materials must be shared with path tracing. Water must
remain compatible with the same authored optical materials.

This roadmap tracks delivery requirements. Detailed measurements and run evidence belong in local
reports. An implementation or synthetic test alone does not close an integration or visual-quality gate.

## Primary delivery task: default baked lighting

Finish the baked runtime before adding renderer features. Ordinary fixed lights should contribute
direct and indirect diffuse through surface lightmaps and probe volumes. Covered receivers should
evaluate only bounded mixed-shadow corrections and independently selected direct highlights;
uncaptured, moving and controllable lights must retain their correct response. Static geometry must
not re-enter moving-caster passes. Runtime profiles now select the consolidated baked policy; analytic comparison remains available.
Selected local-light corrections now skip unit-SH/PCF work when the actual current
world caster set proves a light's moving map empty. Static visibility fallback and
viewmodel self shadows remain independent. Captured-caster counts are reused;
hidden or unbaked actors disable the shortcut. Two-map warm replay controls show
pixel-identical output and lower GPU time; live gameplay acceptance remains open.
See `empty-moving-shadows-2026-10-20.txt` for controls and limits.


Completion requires scalable attributed surface corrections beyond the four retained SH owners,
known probe coverage for selected corrections, and a bounded fallback for uncovered receivers.
The direct-work scheduler now uses immutable sparse chart offsets and explicit output tags;
its existing four-slot adapter runs through the same batching code. Arbitrary overlap, batch
boundaries and 32-bit task-count overflow have CPU coverage. The opt-in indexed directional path
now connects bounded field collection to controller submission/readback, publication and cache
persistence. It reuses retained samples and traces additional positive owners in bounded field windows,
preserves seam donors and refuses incomplete export. Cache import validates the exact owner
manifest and chart rectangles. Publication preserves complete coverage before comparing memory.
Seven-owner runtime tests exercise cancellation, an independently blackened owner outside the
four legacy channels, exact cached restoration and 1x/2x/4x MSAA. Selected aggregate tests also
exercise the extra owner's moving-shadow correction on charted surfaces and a separate dynamic
probe-lit receiver at 1x/4x MSAA. Controlled black correction fields preserve shared aggregate RGB
while isolating the selected response, and exact cache restoration removes that intervention.
The normal path now uses complete indexed directional capture. Aggregate probe correction planning now fills its
existing storage allowance instead of imposing the scalar trace-count ceiling; whole light volumes
remain atomic, and planning runs on the preparation worker. Complete probe availability on one
measured map does not establish map-scale performance or coverage on other maps. Finish those gates before
expanding rollout acceptance, and remove redundant legacy directional scratch/cache layers
only when migration preserves fallback behavior.
Complete directional capture now batches bounded windows of fields with sparse receipt indices,
retained-sample reuse, cancellation and all-or-nothing publication. CPU and hardware tests preserve
the serial coefficients and encoded fields under reordered dispatches. Keep extending saved-map
checks of cache restoration, image parity and bake latency beyond Crossfire. Measure bake GPU time
separately from wall time; normal scene rendering and presentation must not be mislabeled as rays.
Toggleable/styled components must remain independently controllable instead of joining permanent
aggregate RGB. Check water, displacement layers and live-GI transitions, then measure Crossfire,
Construct, c1a0/c1a0d and Steamlab with identical quality and fixed camera positions. Default rollout
requires measured steady-state GPU improvement and visual checks of highlights, wall occlusion and
moving shadows, not just a smaller clustered-light counter.

Complete controllable fixed-light groups replace per-light analytic diffuse under
the canonical baked-lighting policy. Runtime profiles enable baked lighting and
styled indirect components; low-level transport callers retain explicit controls.
Validate independently scaled groups across more maps and keep them separate from
permanent aggregate RGB. Keep arbitrary style identities and runtime weights
engine agnostic. Preserve per-light specular visibility, moving-caster corrections,
cache restoration and unknown-coverage fallback. Use exported response counts to distinguish
this remaining work from selected highlights; fewer clustered lights alone does not prove a
frame-time improvement.
The CPU `groupStyledDirect` compositor now reuses completed per-light surface and probe
SH without launching style-specific rays or allocating dense all-map RGB workspaces.
It declines complete style groups on missing coverage or storage exhaustion. Signed
coefficients, seven-owner overlap and independent group failures have CPU coverage.
Worker publication and runtime selection consume complete direct groups through the
canonical baked-lighting policy. The experimental control remains a lower-level comparison. Aggregate caches retain their existing identity;
groups are derived again after validated reload, while analytic-only probe capture
uses a capability key. Scalar toggles, non-scalar fallback, moving shadows, dynamic
probes, cache reload and cancellation have GPU coverage. Direct groups now combine with optional styled-indirect groups in one bounded sparse
table. GPU coverage includes seven overlapping emitters with preserved total energy
and moving shadows that retain indirect illumination. Construct fresh-bake and cache
reload images agree exactly. Broader map visual acceptance remains open.
The localized Construct bright patches were traced to interpolated atlas page IDs
being truncated into the preceding page. CPU and GPU samplers now round to the
nearest layer, with a failing-before/passing-after boundary regression. Broader
interior and moving-object visual acceptance is still required before rollout.
The private GPU probe-table header now preserves integer offset bits beyond the
old numeric-float precision ceiling; larger styled tables can publish without
changing the saved cache format. Continue receiver-edge and bright-patch sampling
quality work alongside broader map validation.

Consolidate policy while finishing this work. Runtime response classification now has one named-input
decision point for aggregate ownership, required mixed responses, shadow selection and independent
highlight fades. Retain the exhaustive mode as a comparison/fallback until the default passes those
gates; retire overlapping experimental switches with explicit profile migration afterward. Indexed
R8 pages and compressed visibility are alternative storage encodings, not separate lighting models.
Cache import and unknown-coverage fallback are required compatibility paths, not cleanup candidates.
The source/build audit found all 85 Forward+ C++ files listed in the feature build; that establishes
build participation only, not that every internal branch is reachable or necessary.

Avoid assuming that fewer texture fetches make a cheaper shader. A lazy per-fragment attributed-SH
fetch experiment preserved pixels but regressed paired Crossfire GPU timings and was rejected.
Keep shader performance changes behind matched measurements; prioritize removing exhaustive light
responses and their preparation instead of adding more branches inside the local-light loop.

Draw-level ownership now skips unit-direct surface texture reads when no retained light needs
analytic diffuse or an active moving-shadow correction. Unknown owners remain conservative. A
same-binary Crossfire replay improved about 5.3%; warm Construct showed no reliable improvement.
This does not close the overall performance gate: the combined candidate still regresses against
the earlier package. A charted-only shader/pipeline experiment measured no useful gain and was
removed. Disabled indexed directional surface decoding is specialized out of ordinary pipelines;
that alone did not resolve the regression. An indexed full-float probe experiment also left the
combined package slower (about 3.5%) and was removed; removing half/amplitude decode is not a
demonstrated remedy. Next compare the old and current shader paths directly, with controlled
specialization/instruction-cost evidence, while retaining sparse zero fields, exact unknown-coverage
fallback and measured output parity. Avoid another storage-format change without such evidence.

## Current capabilities and open requirements

### Profiling and authoring priorities

Investigate unexpected mid-map rebakes with explicit invalidation receipts: distinguish changed
lighting content, replacement receiver bindings, settings and stale captured lights. Camera movement
alone must not invalidate lighting. Keep genuine edits and late replacement assets correct; do not
silence invalidation by accepting stale bindings. Adapter sky-prop discovery and late material loading
also need controlled live-engine checks.

Light lists already use screen tiles and depth slices. Profile opaque shading, list construction,
shadow work, CPU submission and frame-time spikes independently. Consider receiver depth ranges,
cone bounds and tighter visibility culling after finishing bounded baked-receiver responses. A smaller
light count or faster isolated replay is not proof of improved live-game frame pacing.

The next optimization gates are: establish live fixed-camera pass costs with GPU availability and
resolution receipts; compare shaded and irradiance-only diagnostics to isolate direct response cost;
finish bounded static-light responses and coverage; then test conservative receiver-aware light lists.
Keep shadow-map generation separate from fragment shadow/visibility sampling in reports. Cluster
construction can be inexpensive while the fragment shader still spends substantial time evaluating
its lists. An opaque depth prepass now reuses vertex transforms and alpha/backface rejection before
expensive shading. It preserves draw order and leaves terrain overlays and shadow passes separate;
cutout holes and 1x/2x/4x MSAA have explicit comparison coverage. Continue live-engine and cross-map
validation, including CPU submission cost and scenes with little overdraw. None of these diagnostic views is a
quality-preserving replacement for the shaded renderer.

Build a BSP cubemap authoring tool with live bounds visualization, draggable faces, independent
capture points, translation, rotation, priorities and durable map-specific files. Support import/export
of automatic results so authored examples become a fitting-validation corpus. Game integration must
handle rotated projection/influence bounds and live updates without rebaking unrelated diffuse
lighting. Keep authored overrides optional; improving automatic fitting remains the default goal.

| Requirement | Current state | Remaining proof or work |
| --- | --- | --- |
| Modular forward PBR, clustered lights and 1x/2x/4x MSAA | Implemented | Refresh current-build engine coverage through the integration milestone below |
| World-metric surface charts, atlas budgets and irradiance probes | Implemented; warped boundary fans, modulating decals and lit displacement paint layers supported | Inspect connected-face continuity, seam donors, close decal receivers, indirect noise and budget coarsening |
| Dynamic receiver agreement with surface lightmaps | Budget-fitted grid, bounded recovery of BSP-solid slots, shared-position receiver diagnostics, static sun visibility and sparse local-light visibility; aggregate direct grid lighting and moving corrections tested; sparse attributed unit-direct probe SH now supplies aggregate corrections with the same receiver stencil as baked irradiance, with scalar-only fallback/cache compatibility | Close room-scale/deferred-light coverage and strict attributed reconstruction gaps; validate moving actors across maps using CS2, Half-Life: Alyx and s&box as references |
| Soft finite emitters, attributed direct diffuse and live specular | Implemented | Compare visible penumbrae and light separation in the live engine |
| Receiver-area antialiasing of baked direct shadows | Adaptive sunlight edge refinement integrated; grouped-site local transport available | Integrate local/overflow refinement, improve subpixel coverage detection and inspect more maps/views |
| Aggregate fully static diffuse and compact directional lightmaps | Controller integration retains attributed masks/SH beside aggregate RGB; surface/probe publication, moving shadows and cache reload tested at 1x/2x/4x MSAA; definition edits/removal invalidate stale data until recapture; the default runtime baked policy bounds per-light corrections/highlights on covered receivers and retains analytic fallback; optional adapter bake hints preserve mixed requirements and identify sphere-shaped surface emission; covered receivers cluster only the mixed/dynamic prefix, with the complete light tail retained for uncovered fallback; runtime profiles enable the canonical policy | Earlier all-mixed aggregate alone did not improve steady-state cost; current policy selects mixed slots and independent highlights with timed fades, retains fallback coverage and skips proven-empty moving-map corrections. Broader live receiver coverage, water/probe/RT parity and map quality acceptance remain open |
| Dynamic-object shadows combined with baked static shadows | Implemented; captured static casters excluded; overflow audits classify constant visibility and retain bounded-error palette masks within a memory budget | Confirm retained coverage across maps and bake quality settings; budget-exhausted overflow still requires realtime shadow capacity/range; live receivers, characters and static membership after map changes |
| Indexed visibility storage beyond four channels | Primary and overflow visibility share indexed scalar records, using shared R8 pages or compressed masks; publication can omit the legacy visibility layer when total GPU allocations shrink; worker conversion preserves cache identity and source layout | Storage does not reduce visibility ray counts; broaden map-level latency/coverage validation of the integrated mixed-selection and aggregate policy |
| Automatic parallax cubemap bounds with tolerance for room details | Coverage-based selection and optional 32-capture capacity; receiver bounds contact nearby axis-aligned room surfaces independently of inset projection boxes | Limited budgets still leave rooms uncovered; review transitions, rotated rooms and reflection leaks across multiple maps |
| Rough SSR, optional hardware reflections and live GI | Implemented | Refresh actual-engine toggles and shared-scene lifetime coverage; continue measured performance work where needed |
| Shared USD/PBR material replacements | Implemented; plain Source roughness fallback corrected | Refresh late material arrival and broader material/BRDF parity in the current engine build |
| Water optics, normals, HDR refraction, SSR/RT and underwater composition | Implemented; enabled for new settings; lazy refraction snapshots | Broader underwater/lifecycle validation, noise and live visual comparison with path-traced material response |
| OIDN indirect-lightmap filtering | Implemented; bounded real-chart CPU/CUDA calibration, with independent CPU chart queues when an additional measured margin justifies them; explicit device/thread overrides retained | Connected-seam review and lower-sample quality comparisons; distribution of optional dependencies |
| Usable build and deployment | Feature branches/remotes retained; Windows runtime bundler available | Remaining platform/dependency coverage, lifecycle smoke tests and final delivery instructions |

CPU queue checkpoint (2026-10-20): chart-heavy workloads that already favor CPU
filtering can compare four independent queues against serial filtering on copied
real charts. Selection requires a further 25% projected saving, at least 16 logical
processors, and no explicit OIDN thread override. Queues share a nominal budget of
32 threads and retain separate chart images, cancellation and joined lifetimes.
Controlled replay pairs reduced main denoising from 19.1 to 11.4 seconds on
Crossfire and 16.6 to 10.5 seconds on c1a0d. Styled fields retained serial filtering
where trials did not justify parallelism. A temporary Crossfire verifier compared
all 3,832 main-lightmap packets with serial CPU filtering on identical inputs,
bit for bit; that verifier is not part of production. These are bake measurements,
not live frame-rate claims. Existing baked caches remain compatible.

Forward+ displays a passive map-lighting indicator while preparing or baking, even
when settings are closed. It shows the current stage's work counts when available
and a spinner through stages without a known total, including finalization. Probe
completion is not treated as overall bake completion. The indicator disappears
when baked lighting is active; a failed bake instead displays its failure message.

## Delivery milestones and checkpoint history

The current defaults are described above. These milestones also retain earlier
implementation checkpoints; their superseded stages are not separate default
renderer paths. Open acceptance work remains part of each milestone.

1. **Replace exhaustive overflow baking.** Complete indexed static local-light visibility for probe
   receivers; static sun visibility already uses the shared probe interpolation, with moving-caster
   shadows and complete shadow maps retained for uncovered receivers. Aggregate ordinary fully static
   direct diffuse with indirect lighting; retain separate light identity only where mixed lighting needs it.
   Aggregate mixed probes now retain nine signed unit-direct coefficients per selected probe/light
   pair alongside scalar visibility. This removes analytic diffuse reconstruction from covered moving
   corrections, without a four-light probe ownership limit. Directional payloads remain bounded by the
   existing storage and pair budgets and use the existing connection rays; scalar-only policies retain
   their cheaper transport. Probe GPU publication now indexes nonzero unit-direct fields separately
   from scalar masks: exactly black fields share index zero, while signed nonzero fields use nine
   amplitude-scaled half coefficients. CPU/cache values stay full precision, with unchanged trace
   work, pair selection and coverage rules. CPU bounds/cache tests and shader compilation pass;
   GPU interpolation, mixed-shadow images and cache restoration are now covered by the indexed
   surface/probe tests described above. The four-owner adapter remains a compatibility fallback,
   while the normal runtime uses indexed directional fields beyond that limit. Cross-map
   normal-mapped actor comparisons and preparation/runtime acceptance remain open. Select mixed lights
   automatically from legacy inputs, preserving important emitters and reusing mask channels where
   receiver coverage permits. Define a correct fallback for overlapping mixed lights instead of
   silently losing their shadows. Keep camera-dependent specular visibility and dynamic-object
   shadow behavior explicit: fully baked diffuse cannot supply either by itself. Evaluate compact HDR
   irradiance plus lower-resolution directionality against the current SH representation before
   replacing it. Require equal-energy, no-double-counting, wall-occlusion and moving-caster tests,
   followed by measured fresh-map bake time, GPU memory and close normal-mapped surface comparisons.
   Keep scalable scalar visibility separate from directional correction storage. A single-channel
   array can index/reuse many chart/light masks; that does not require retaining nine full-resolution
   coefficients for every contributing light. Audit assigned chart rectangles and constant-clear
   coverage before choosing correction allocation, and keep packing/alignment costs separate from raw
   rectangle sums. The optional `REMIX_DEBUG_DIRECT_OWNERSHIP` file report captures this accounting on
   both fresh and cached publication without modifying the bake or selecting a new lighting policy.
   Primary visibility now joins the indexed scalar records at publication. Primary half values round
   to R8 (at most half an R8 step plus float rounding), with lossless encoding after conversion;
   existing overflow bytes and constant visibility remain unchanged. The publisher compares actual
   lookup/page allocations and unbound-image atlas requirements, selecting a nineteen-layer GPU
   atlas only when it saves total memory. Budget/device-capacity failures keep the complete legacy
   layout. Cache and logical diagnostic atlas data stay at twenty layers; segmented upload skips
   visibility without copying a second full atlas or invalidating existing disk caches.
   Surface directionality now has a lossless sparse chart/light representation with no four-owner
   storage limit: empty and constant 8x8 blocks share compact records, while dense chunks retain raw
   half fields. Worker conversion preserves every currently retained coefficient and never modifies
   the cache atlas. Publication compares all four visibility/directionality layouts using unbound
   image memory requirements, allocates only the winner, and can omit the nine local directional
   layers independently of visibility. Budget/device failures retain a complete atlas fallback.
   CPU codec, ownership, multi-page upload and real-cache bit comparisons pass; shader compilation
   passes. GPU decoding, filtering/image agreement, lifecycle and performance checks are still
   pending. Transport and cache ownership still retain four directional owners: capture and persist
   additional fields before claiming that the four-owner limit is removed end to end.
   Geometry-emitter hints alone do not address maps dominated by explicit point/spot lights. Extend
   aggregate ownership to those fixed emitters while retaining compact, selected per-light specular
   visibility and moving-caster corrections; avoid restoring exhaustive evaluation for every baked
   emitter. Validate this separately on maps with no texture-emission lights, including Steamlab.
   The opt-in `experimentalSelectedBakedLights` runtime policy now keeps aggregate corrections and
   direct highlights only for lights holding a mixed-shadow slot (including outgoing fades).
   Unselected fixed lights retain aggregate diffuse and environment reflections; uncovered receivers
   retain the full analytic fallback. Styled/moving lights and adapter-required mixed lights are
   exempt. This reuses attributed bake data and does not reduce its preparation cost. Broader quality,
   independent specular selection and cheaper on-demand correction visibility remain useful comparisons.
   Keep that lower-level option disabled; the canonical policy uses independent highlights.
   F12 now exposes the default-on **Use baked lighting** control. It derives
   aggregate diffuse, baked geometry emitters and selected mixed responses without overwriting
   the underlying profile preferences. Map baking and baked direct diffuse must be enabled;
   supported live GI temporarily pauses baked lighting. Disabling it restores the requested policy.
   Profiles migrate legacy preview settings to the canonical `bakedLighting` control;
   explicit canonical zero retains analytic comparison.
   The runtime independently selects up to 16 attributed fixed-light highlights, with timed fades
   and captured-emitter keys rather than potentially shared adapter light IDs. A separate GPU
   highlight weight preserves mixed-shadow strength when the selections differ. Outgoing highlights
   retain their bounded assignments until their weight reaches zero. Canonical profile keys
   `bakedHighlightLights` (0..32) and `bakedHighlightFade` (0..10 seconds)
   control that budget and fade independently of shadow maps. Pure baked geometry emitters still
   omit analytic highlights. Camera-distance/influence ranking is an initial approximation; test
   live-map highlights, leakage and frame cost before expanding quality acceptance.
   Transport uses a separate 112-byte light contract, preserving its existing shaders and cache
   identity when the runtime light contract grows to carry independent response weights.
   First-bake denoising still competes with rendering. The replay host can isolate that contention
   using `replay.denoiseFrameDelayMs`, a default-off diagnostic restricted to the denoising stage;
   it does not change the installed game's cadence. Investigate scheduling before adopting a frame
   cap. Completed caches now move into CPU-only immutable snapshots after upload preparation,
   independently of ready publication. Persistence retains at most one active and one pending map,
   with a 4 GiB charge budget for owned payloads (shared captured assets are not duplicated or charged).
   Admission failure skips optional persistence without blocking lighting. Cancelled/incomplete bakes
   never enter this queue; completed old-map writes remain valid across map/renderer changes.
   Slow/failed writer tests cover nonblocking admission, bounds, failure recovery and shutdown drain;
   SDK shutdown joins the worker before DLL unload and a later startup can restart it. Cache-reload
   tests wait for persistence explicitly. Continue live-map lifecycle and storage-failure coverage;
   orderly shutdown can wait for accepted writes, and the filter/render contention remains unresolved.
   A highest-priority CUDA stream did not reduce filtering time in the exercised Vulkan-rendering
   workload; retain ordinary stream scheduling. Next investigate filter/image work organization,
   preserving chart isolation, directional coefficients and cancellation, before changing quality
   settings or introducing an interactive frame-rate tradeoff.
   The automatic CUDA path now submits bounded queues of independent chart filters, with one
   host upload/readback per queue and an additional device arena capped at 8 MiB. Device copies
   keep every chart's filter input and result separate. CPU, pageable and oversized work retain
   ordinary submission; unsupported batching or arena allocation falls back without changing
   filter models or samples. Chart orchestration, private filter ownership and driver graph
   operations live in separate modules. Constant fields, changing band masks, partial queues,
   directional reconstruction and cancellation remain covered by ordinary-dispatch comparisons.
   This is an incremental reduction; substantial filter/render contention remains. A first
   version capturing host copies regressed and was rejected. Chart-heavy default-CUDA workloads now
   sample copied real chart packets on CPU and the default device. Three size bins and a bounded input
   budget limit calibration work; a CPU override needs a projected margin above 35%. Small workloads,
   maps dominated by large images, other default device types and explicit `OIDN_DEFAULT_DEVICE`
   settings keep normal selection. Trials do not modify captured lighting or production work counters;
   candidate arrays outlive queued execution and cancellation drains through the existing filter owner.
   Calibration uses no persisted hardware preference and changes no rays, model quality or cache schema.
   Continue device/workload comparisons
   and live-map responsiveness checks; do not infer game FPS or universal speedups from replays.
   Timed visibility batches now grow within an eight-row rectangular dispatch and a guaranteed
   128 MiB maximum result-buffer range, reducing frame submission overhead without changing quality.
   Scalar classification now selects positive direct owners before a separate directional pass.
   This removes the full-map directional scratch array and readback for discarded candidates;
   encoded caches remain identical in the exercised fixtures and map replays. Extra directional rays
   offset the transfer savings in measured wall time. Retained directional work now uses an immutable
   receiver-prefix index and bounded generated batches instead of an expanded pair list. Continue
   reducing exhaustive visibility pairs while preserving the same shadow and direct ownership guarantees.
   Finite-emitter cone bounds reject chart/light links only when every biased receiver and seam donor
   lies outside the emitting cone, with sphere/triangle extent and rounding margins retained. These
   are zero-emission exclusions, not invented occlusion proofs or a fixed influence-radius cap.
   Runtime shading also skips filtered visibility/shadow work for a zero analytic cone response when
   no retained direct coefficient needs evaluation; finite emitters crossing the cone edge keep their
   baked directional contribution even when the centre approximation is zero.
   Supported live RTGI temporarily requests attributed static visibility instead of aggregate bake
   policies, because its analytic primary lighting cannot use aggregate irradiance for occlusion.
   Raster preferences remain intact when switching back. This is a correctness fallback; a compact
   shared visibility path for live GI remains future work. Transport checks inspect linear channels
   independently of Prism, whose saturated-primary compression intentionally mixes display channels;
   live-GI coverage separately checks the shared display transform.
2. **Complete live GMod integration.** Pool startup, a fresh bake, an exact cached reload and a moving
   actor have been exercised in the isolated installation. Continue with a solid receiver for mixed
   shadows beyond the exercised indoor sun case, below-water views, renderer/RT switches and a map reload/change. Record binary hashes and
   dependencies. Require actual
   engine logs, active baked lighting and captures; investigate crashes, input restarts, missing assets
   or new validation errors before extending coverage. Earlier live-game logs are historical evidence.
   The refreshed paired testing install also publishes the same Construct bake on consecutive
   controlled launches with disk-cache hits and no GPU bake work. Keep first-start input stability,
   late material replacement and addon-heavy sessions as separate cache gates; an unchanged warm
   capture does not explain an earlier differing scene fingerprint or prove all-map cache reuse.
3. **Close visible lighting-quality gaps.** Use fixed map views and a high-sample reference to inspect
   connected chart seams, static/dynamic shadow composition, indoor/outdoor probe transitions and
   parallax bounds. Compare lower indirect sample counts separately from direct-shadow sampling.
   Keep changes tied to an observed artifact and a repeatable comparison.
   Styled/toggleable BSP lights now capture immutable intensity envelopes and live identities for
   charted unit direct/static visibility. Toggles reuse that capture; local-map selection has timed fades.
   Complete style-dependent indirect components for surfaces and probes, indexed static visibility on
   dynamic receivers, and reflection relighting. Verify actual engine on/off/flicker and initially-off
   loads across maps. The direct-only stage does not complete styled lighting or identify ceiling artifacts.
   Attributed SH/radiance transport and authored-envelope scalar resolution are available internally.
   They still need compact chart/probe storage, component caches and coordinated runtime publication.
   Current component passes repeat tracing; require measured work/memory bounds before default use.
4. **Finish material and water parity.** Exercise authored/replaced normals, roughness, emission,
   transmission distance and IOR in the live engine. Document supported approximations and repair
   discrepancies that break compatibility with the shared material system.
5. **Package and deliver.** Check clean builds with and without OIDN, include required runtime plugins
   and notices, document settings and defaults, and repeat the integration smoke test from the delivered
   layout. Keep the upstream `main` branches and the user's feature-branch remotes intact.

## Viewmodels and authored reflection bounds

Opaque Forward+ viewmodels use an independent MSAA HDR/depth target. The authored camera
correction retains physical world positions for the shared probe and material shaders. Composition
overlays the weapon after world water/reflections and before the shared display transform; exposure
continues to meter the world. World depth, reflection guides and ordinary shadow caster lists remain
independent. Raster-only and otherwise unsupported attachments keep the compatibility overlay layer.

Viewmodels have a dedicated close-up sun cascade. Its XY footprint follows weapon receiver bounds,
while world caster bounds extend depth without reducing XY density. Relevant frozen and current
world geometry plus weapon geometry cast into this map, sampled only by viewmodel receivers.
This deliberate close-up static-caster pass is separate from the fully baked world's shadow policy.
Targets allocate only when first needed and are queue ordered; posed geometry and constants retain
frame-slot ownership. Local-light self-shadows use an independent dynamic-only depth array for the
selected light slots, combined with existing world visibility only by viewmodel shaders. No duplicate
static cache is allocated for these self casters. Probe-only readback compares the same surface in
viewmodel and world ownership with live lights and ambient fallback removed, and checks that ownership
changes preserve the bake snapshot. Viewmodel environment reflections reuse the world's published
parallax cubemaps, roughness mips, Fresnel response and visibility-filtered sky fallback. They remain
independent of world SSR guides and use the existing reflection-enable policy. Metal/dielectric/rough
surface readback compares this path with world shading without live lights. Continue with actual
weapon/FOV/animation checks and GPU profiling in GMod. The sun/point-light overlay fixtures cover
MSAA, world and self shadowing, wall overlap, removal, fallback attachment
ordering and unchanged world pixels; they do not establish complete live-game viewmodel parity.

Lazy material replacements may change global mod generation without changing a map receiver.
Retained replay preserves a receiver's revision when its mesh, original dependencies and resolved
shading materials remain identical. Merged replacements retain their pointer-validated cache
entries when unrelated replacements arrive. Actual receiver replacements still invalidate baking.

The authored reflection reference contains 14 combined probe volumes, distinct from ordinary
capture points. Its asymmetric corridor bounds and off-centre capture origins are useful test
cases. Geometry verification now matches the reference to hls00amrl with identity coordinates;
the earlier hls01amrl comparison used the following map. Balanced interior seed competition fixes
diagonal-room slivers without replacing the established fits or authored tie preference. Continue
room-local visual comparisons and Construct coverage/doorway overlap work. Keep capture position
independent of volume centre; the aligned reference does not establish complete automatic bounds.
A six-axial-ray projection trial improved mean reference-box IoU only from 0.223 to 0.290
and still crossed doorway boundaries by over 2000 Source units at individual sites. It is not
a production replacement. Next candidates must establish receiver visibility and room enclosure
separately, with actual Construct coverage gains; see cubemap-axial-comparison-2026-10-20.txt.

## Work discipline

New standalone tooling should prefer C#. The current native Qt cubemap workbench remains usable;
a larger Hammer-like editor for authored Remix replacement layers is a later project. Its future
design should retain independent capture points, oriented volumes, live publication and portable
authoring documents. Do not expand that editor project at the expense of the renderer quality and
performance gates above.

Each checkpoint records its changed files/commit, evidence, limits, and the next action. A failed
experiment remains a failed experiment. A running process is tracked by its actual handle until it
finishes. Performance improvements include before/after configuration and output comparisons.
Milestones close against their stated gates; newly useful features do not replace unfinished gates.
