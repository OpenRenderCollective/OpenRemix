# Forward+ delivery roadmap

The intended result is a cheaper, Source-2-style rendering backend for GMod: forward PBR shading with
MSAA, hardware map-load surface lightmaps and probes, attributed baked direct lighting, soft static
shadows combined with dynamic-object shadows, automatic tolerant parallax cubemaps, rough SSR, and
optional hardware reflections/GI. Replacement materials must be shared with path tracing. Water must
remain compatible with the same authored optical materials.

This roadmap tracks delivery requirements. Detailed measurements and run evidence belong in local
reports. An implementation or synthetic test alone does not close an integration or visual-quality gate.

## Continuation status (2026-10-22)

The separate GMod testing install now uses native `3c7c428` and adapter `d7894f6`; it includes the
current clustered fallback, shared sphere-bound loads, adaptive probe reconstruction and runtime
publication fixes. This makes the feature branch available for testing, not an accepted general
performance or visual-quality release. Existing dependencies, settings and authored volumes are
preserved with a rollback checkpoint. The previous uniform-grid install remains a historical baseline.

Runtime performance investigation must distinguish completed-bake gameplay from bake transport and
startup. The renderer already provides an opaque depth prepass and tiled, depth-sliced light lists.
Next isolate the dominant opaque receiver work with bounded fragment/receiver diagnostics, preserving
ordinary draw order and checking omitted ranges. Investigate the observed increase in local-shadow
GPU time when the independent baked-highlight budget is reduced: compare selected emitter identities,
caster submissions and timestamp boundaries before attributing the difference to extra shadow work.
The Crossfire check now confirms unchanged shadow identities and submissions across highlight budgets;
the timestamp shift alone is not evidence of additional caster work. Local point shadows now clip
casters against individual cube faces, retaining seam-crossing and offscreen actors. Frame reports
expose per-slot identities and recorded frozen/moving draws for continued profiling. Next isolate
covered versus uncovered probe receiver cost and the remaining surface-lightmap shading cost.
Coverage views locate some uncovered distant geometry. The clustered baked tail shares one ordered
list with the runtime prefix, so `receiverLightCount` can use the complete spatially culled list on
uncovered receivers. Complete analytic traversal remains available on overflow; covered receivers
retain independent prefix counts and overflow. View-space spheres are prepared
once per uploaded light, including affine-view bounds. Keep mapped receivers on their short prefix;
do not omit unknown lighting. Core GPU membership and live integration checks cover this path.
Cooperative sphere-bound loads reduce redundant light-buffer access during cluster construction.
Fixed-view renderer replays cover Crossfire, Construct and c1a0; they do not establish live gameplay
performance. Continue broader maps, moving/armed views and worst-case build/storage checks before
deployment acceptance.
Crossfire can remain limited by host time after GPU time falls: profile the engine/adapter caller and
native preparation separately before attributing the remaining frame cost to light shading.
Caller-stack sampling now locates repeated sleeps in the original Source shader backend while the
native render thread waits for work. Treat this as a host synchronization lead, not proof of expensive
CPU computation. The initial `mat_forcehardwaresync=0` anomaly coincided with lost focus and is excluded.
A subsequent startup-only comparison verifies focus, effective settings, clocks, captured inputs,
rendering counts and near-identical world images. Disabling sync trades host delay for greater native
GPU/fence time and helps only one tested direction; keep the default unchanged. Lua attempts to toggle
this setting did not apply and are rejected as comparisons. Next isolate D3D event-query polling from
occlusion and readbacks, preserving actual completion rather than removing synchronization wholesale.
The adapter now offers opt-in per-vtable query instrumentation, separating event and occlusion
issued/pending/failed/flush counts and driver-call time. Live checks locate frequent pending event
polls in the slower host view; driver-call time itself remains small. Profiling-on/off world images
and rendering counts match. Keep instrumentation timings separate from performance acceptance.
Two bounded event-retry policies preserve completion/errors and flush progress but fail to show
a repeatable frame-time win. Higher poll counts rarely complete the slow-view event before falling
back to the engine sleep, and that view regresses slightly. Both tested policies are rejected;
the experimental runtime path, helper, tests and retry-only counters were removed. Keep the useful
query profiler and existing wait defaults. Return to armed/moving gameplay reproduction and dominant
opaque/viewmodel shading; longer or adaptive waits need new evidence and a CPU/overlap justification.
One live Construct check exited in the original shader API's filesystem worker after bake publication,
with an invalid virtual-call target at `shaderapidx9_orig.dll+0xba1f`. An identical rerun completed.
This is unresolved, not an accepted intermittent failure or proof that the native change caused it;
preserve the crash evidence and investigate interface/texture lifetime if it recurs.
Also audit cold cubemap capture repeatability: distinguish unchanged packed inputs/sites from identical
captured radiance before claiming a byte-identical full bake or an isolated shader comparison.
Changing that budget must not rebake or change published lighting buffers. Keep the slow Crossfire
gameplay report open until its actual settings/location/weapon workload is reproduced; two fixed
unarmed views do not establish broad performance acceptance. Retain highlight and shadow defaults.

Latest live moving-prop checks in Construct confirm valid probe coverage on the barrel and neutral
crate while their appearance remains too dark. Coverage alone does not close probe quality; compare
lighting components, receiver normals and reference density before changing brightness globally.
Live component captures now show correct neutral-crate albedo; bypassing normal maps does not remove
the low irradiance, and adding every stored controllable group has negligible effect at sampled prop
faces. A matched-input 131072-record/1m-request bake versus the 32768-record default changes faces
in opposing directions and leaves the visible problem unresolved. It also exposes 30 analytic fallback
lights under existing table budgets. Do not raise the global default based on this experiment. Obtain
direct transport references at actual prop-face positions, then assess interpolation, directional SH
and local spatial support. Below-floor contributors alone are not proof of the cause; in the sampled
default stencil removing them would reduce energy further. Keep brightness compensation out of this
investigation until a reference establishes the missing energy.
Bounded hardware point references now provide that incoming-SH comparison under identical captured
scene inputs: at representative faces the default field is roughly 35% low on the crate side and
72% high on the barrel top, while the crate front is about 7% low. The fronts remain dark in direct
4096-sample transport, so this is not uniformly missing probe energy. The diagnostic leaves published
irradiance/style/visibility buffers byte-identical to an ordinary bake. It is opt-in and separate from
placement/publication. These are incoming-SH comparisons, with primary sunlight separate and local
direct membership policy-dependent, not final-pixel ground truth. An 8-sample live-GI visual check was
noisy and its alternate bake unfinished; do not use it as quality acceptance.
The bounded reference now also supports independent cosine hemisphere integration. At the same five
sites, 4096 versus 8192 rays differ by about 0.3..3.9% in luminance. Fronts remain dark; the default
field is about 68% high on the barrel top and 33% low on the crate side against the 8192-ray result.
Replacing sixteen dominant neighbours with high-sample SH improves the crate side but worsens barrel
excess, so more samples alone is insufficient. The barrel mixes about 27% coarse lighting with much
brighter sky response; removing normal-facing weights also worsens fronts in an offline experiment.
Keep both defaults unchanged. Next check coarse-contributor geometry visibility and improve the
coarse/fine interpolation or directional visibility while preserving continuity and runtime cost.
An ordinary-visible-brush BSP ray check now finds all barrel-top coarse contributions blocked by
world surfaces, plus roughly 41..44% of crate-front/side weights crossing the floor. This is a
world-brush subset oracle, not the runtime's complete geometry. A half-precision directional-depth
prototype exposes another issue: separately normalizing tiny residual blocked coarse support can
restore its full blend share. Confidence-aware coarse/fine blending improves that subset experiment.
An 8x8 field with ordinary probe RGB reduces barrel-top error from about +68% to +17% and crate-side
error from -33% to -18%; other faces still need work. Exact visibility plus high-sample neighbours
performs better, but neither a higher sample default nor removing facing weights alone is justified.
An opt-in native implementation now produces bounded GPU directional depth and shares confidence
reconstruction across CPU/GPU sampling. Enable only for investigation with
`REMIX_DEBUG_DIRECTIONAL_PROBE_DEPTH=1`; the default remains off. Each probe adds 256 bytes
(about 8 MiB at 32768 probes), with cache policy revision 26 and explicit snapshot ownership.
Matched live Construct samples reduce barrel-top error from +68% to +17% and crate-side error
from -33% to -18%, but crate-front error worsens from +8% to -12%. This does not close quality.
Cold capture costs about 2..3 ms total GPU time in that scene; a disk reload skips capture and
reproduces published irradiance, style and local visibility buffers byte-for-byte. Whole-frame
runtime cost has now been checked with an off/on/on/off cached Construct sequence at
1920x1080, 4x MSAA and full-resolution SSR on RTX 4090. The two-run means add about
0.10 ms GPU time without a weapon (4.22 -> 4.32 ms), and 0.19 ms with the physics gun
(5.37 -> 5.56 ms). These are one fixed hall view, not broad-map performance acceptance.
Every run loads from disk without bake transport, preserves the scene input identity and
has zero static-shadow updates; repeated runs of each policy publish byte-identical buffers.
Sample-window graphics clocks remain about 2760 MHz. Broader map/moving-viewmodel checks remain.
Cache retention accounting includes depth, lookup and origin storage. Seam, unknown-coverage,
styled/indexed table offsets, adaptive confidence and hardware producer tests cover the new layout.
Directional probe depth remains disabled by default in the testing install until those gates pass.
Further offline contributor checks still find substantial residual through-floor support with
the 8x8 field. Filtering visibility per octahedral tap instead of filtering moments trades
leakage for worse dark-face error; do not adopt it globally. High-sample neighbour substitutions
help the crate side but not all faces. Next investigate near-surface spatial placement and
reconstruction against the independent hemisphere references, retaining the fixed memory budget.
The legacy six-axis stencil now applies one-sided confidence to suppress weak coarse fallback;
it does not increase the coarse share over valid fine support. Directional-depth confidence remains
symmetric. This reconstruction change requires no extra probe records, rays or cache revision;
broader prop/viewmodel quality and whole-frame cost remain acceptance work. Do not treat it as
closure of the directional-depth candidate or as a general valid-slot relocation rule.
A bounded physical support-halo experiment fits 48 rather than 33 cells at 2 m spacing within
the same 32768-record budget, including the test hall. Four sampled face errors improve, but
the crate front becomes substantially darker; the change is excluded from production source.
It also exposes budget coupling: deferring one member of a controllable group can leave all
30 members analytic. Probe-volume planning now selects complete controllable groups together,
ranking pair cost per member and repairing a nearly fitting group by evicting a selected suffix
only when it retires strictly more lights. A final fill pass reuses remaining capacity.
The same constrained Construct experiment then drops from 30 analytic lights to zero while
retaining 135/136 complete volumes within the existing 64 MiB allowance. This is a coverage
and ownership result, not a broad FPS claim. Oversized groups still keep complete runtime fallback.
Grouped probe policy version 2 intentionally separates older caches whose directional data may
have stranded a component; the binary cache layout and public API are unchanged. Ungrouped
planning retains its original cheapest-volume order. The testing checkpoint includes this fix.
Optional `REMIX_DEBUG_BAKE_TIMING=1` output now timestamps changed progress messages so
CPU stages and trace batches can be joined to frame telemetry without affecting bake identity.
A matched cold Construct pair attributes about 14 s to base lightmap denoising, 9 s to
per-light visibility and 4 s to styled encoding/denoising in an approximately 52 s bake.
The main visibility pass now reuses the validated receiver table already used by overflow
tracing, removing repeated expanded-point vectors and validation. Receiver storage remains
bounded to 128 MiB with the ordinary path on a declined allocation. In that pair, visibility
phase CPU recording falls from 9.8 to 3.7 ms/frame; total bake time only changes 52.6 to
52.2 s, so this is not a broad bake-throughput win. Visibility/styled tables match exactly;
raw probe RGB differs by at most 2.4e-7 and the checked static world crop by at most two
display code values. Next prioritize denoising throughput and remaining GPU/visibility work.
A matched two-queue CPU denoising experiment on the 13900K is rejected: with eight threads
per queue, base filtering rises from about 14 to 19 s and total bake time from 52.2 to
57.2 s. Four queues remain selected by the existing calibration; no queue override remains
in production code. Consider reducing provably irrelevant visibility candidates next.
The existing runtime light locator uses emitter centres/nearby samples; it is not a proof
that a whole finite emitter and all biased/dilated surface receivers are disconnected.
Any bake-side visibility-partition pruning needs conservative volume coverage, explicit
unknown/embedded-light fallback and agreement with replacement geometry before storing black.
The read-only audit now exists: set `REMIX_DEBUG_BAKE_VISIBILITY_REPORT` to an absolute JSON
path for a cold bake, then run `python scripts/experiments/visibility_partition_audit.py CAPTURE
--output REPORT`. It records whole emitter bounds, full chart/donor bounds, biased sample-point
bounds and completed visibility classifications. `--self-test` checks boundary, unknown-region,
asymmetric-matrix and zero-weight clear-sentinel handling. No PVS rejection is enabled.
Construct's broad-bound audit predicts 10854 disconnected pairs; 436 carry the baker's clear
classification. The biased-point alternative predicts many more but is not a full receiver-area
proof. Clear can also mean no emitter samples had positive weight, so these counts alone cannot
establish either broken visibility or safe pruning. No isotropic-sphere clear conflict is found
in that capture. Future PVS-based baking must also add matrix bits to cache identity; current
bake content hashing includes partition geometry/cluster labels but not visibility rows.
The audit exposes a separate analytic omission: finite cone rejection previously handled only
positive outer cosines. Its conservative signed-distance bound now also covers wide cones and
rounded negative `cos(pi/2)` values, while preserving unrestricted emitters, finite extents,
backward oblique connections and numerical tangency. Construct removes 2010 extra candidate
links and about 864000 visibility points; a matched cold run changes 52.2 to 51.8 s, with the
checked shaded world crop within two display code values. Treat this as bounded work reduction,
not a broad bake/FPS win. Existing completed caches remain valid; no layout or cache-key change.
Camera motion also refreshes frozen sunlight cascades and prepares the captured world casters.
The CPU projection cache now preserves the exact rendered projection for subtexel fit noise.
Targeted CPU and sunlight/local-shadow GPU checks pass. Opposite-order moving-camera comparisons
reduce sun-shadow cost most clearly outbound, with a much smaller return-leg benefit; whole-frame
FPS improvement is not established across maps. Stationary world images remain within two display
code values per channel. The testing checkpoint includes this; broader motion/visual acceptance remains open.
Keep necessary uncovered-receiver fallback and offscreen moving shadows intact.

Work resumed after the handoff. Neither official feature branch had new contributor commits, and both
already contained their official main branches. The immediate investigation is measured fragment work
and depth-prepass effectiveness before another shader optimization. Optional bounded fragment and
precise passed-sample counters are available; they do not alter draw order or the public API.

Ordinary-vertex receiver specialization now removes impossible surface-atlas work without adding
modules or reordering draws. Two opposite-order Construct replay batches and targeted GPU readbacks
support keeping it; broad map and live-game acceptance remain open. The replay
host also rejects renamed runtime DLLs so missing adapter extensions cannot masquerade as a slow bake.

Adaptive probe light volumes now use each record's conservative support instead of assigning coarse
reach to every fine-only probe. Construct retains all eligible light volumes within its existing budget,
allowing the white-room controllable group to publish. CPU stencil-parity and grouped GPU checks pass;
a controlled cached replay improves over the preceding adaptive checkpoint. Cache policy revision25
requires a deliberate first-upgrade bake. Release still needs broader map/live-quality checks and a
live comparison against the earlier uniform checkpoint; the older regression figures are historical.

That comparison now favors the current build in the fixed Construct hall view. This is evidence for
that camera, not live-game FPS parity or a release gate for every map. c1a0, Crossfire and c1a0d also
publish and reload matching caches in first-spawn replay views. c1a0 exposed an empty coarse-seed
failure: adaptive placement now retries the normal full-density policy, while sparse density search
preserves its last verified candidate. Real empty partitions still fail and cancellation still propagates.
The isolated test content needed its shared Source assets mounted for c1a0d water materials; this is
separate from renderer correctness. The reported locker-room noise and live frame pacing remain open.

Lost Coast and Steamlab now pass fixed-view publication/cache-reload checks too. Steamlab replay
needed its game-specific VPK mounted; the Source adapter now supports opt-in read-only game roots.
This is separate from validating the user's installed live-game case. Optional reflection-stage timers
separate guide/hierarchy/screen-trace/filter cost from the much heavier hardware-reflection path in
one measured view. Full-resolution SSR remains the default. A repeated live installed-build profile
of the verified Construct skylit hall now identifies opaque GPU rendering (including its depth prepass)
as the largest measured cost; CPU recording is smaller and fence waits overlap GPU work. Static
shadow updates remain zero after baking, and camera turns preserve the active bake. This does not
reproduce the reported Crossfire slowdown or accept the candidate: compare matching live settings
and candidate/installed views across maps next. Live hall sessions now share an identical captured
scene identity and retain it through camera turns. A small candidate slowdown with automatic cubemaps
means the replay gain is not a live performance acceptance. The test install also has a zero-volume
Construct authored sidecar, which explicitly disables local cubemaps. Isolating that override restores
53/54 automatic captures in installed/candidate builds. F12 now identifies the active probe source and
explains an empty authored set; the saved file is preserved. Probe bounds/blending quality remains open.
Startup cache-identity differences outside the verified hall sessions still need fingerprint diagnostics.
An initial live Crossfire courtyard pair favors the candidate, with opaque shading still dominant, but
does not reproduce the historical slowdown. Opposite hall and courtyard outcomes require broader
controls and receiver/highlight profiling, rather than a global quality reduction or release claim.
The corrected Crossfire opposing-view check also favors the candidate, but has variation and partial
CPU/engine pacing. Highlight-budget isolation in the live hall retains the bake and four mixed-shadow
entries; removing highlight-only entries reduces some cost, but leaves most opaque work. Defaults are
unchanged. F12 and the probe report now separate positive highlight and mixed-shadow roles, which can
overlap. Raw GPU deltas are not interchangeable with FPS.

A cached live hall session also keeps its active bake and published content through verified vanilla
physgun/crowbar/pistol switching and weapon removal, including transition frames. This does not close
addon weapons, movement or other-map rebakes. Actual viewmodel rendering adds a substantial measured
cost, so isolate its SSR, shadow and fragment work next; retain the overlay, world-probe lighting and
isolated self-shadow requirements. Its appearance still needs comparison against the reference path.
The physgun SSR isolation leaves substantial viewmodel color/resolve cost. Conservative CPU-prepared
cubemap candidate masks now reduce that work without changing probe projection/blending or full-res
SSR. Unknown receiver bounds retain the full set. A 64th-probe GPU fixture matches complete masked and
unculled frame hashes at 1x/4x MSAA; two live renderer orders favor the mask in the measured hall. This
is not broad map/weapon performance or visual acceptance; continue the remaining quality matrix.


## Map acceptance matrix

Keep a varied map set rather than accepting performance or lighting from Construct and Crossfire
alone. The additions below are planned coverage, not completed test results. Verify the actual loaded
BSP, content mounts and required addon assets before accepting a run; a missing map or fallback load
does not count.

Exclude alt-tabbed or unfocused intervals from accepted FPS comparisons; the user reported tabbing
out during an earlier run. Repeat any affected timing window with the game focused.

| Map or map family | Coverage to exercise |
| --- | --- |
| `gm_construct` | Skylit main hall, white-room controllable lights, terrain, prop probe lighting, cubemap transitions and mixed shadows. |
| `crossfire` | Light-heavy opposing views, armed/moving gameplay, host synchronization and opaque/viewmodel cost. |
| `c1a0`, `c1a0d` | Interior lighting, intersecting emitters, locker-room noise, weapon changes and bake stability. |
| `d2_lostcoast`, `dm_steamlab` | Previously reported bake failures, light leaks, transport quality and cache restoration. |
| `gm_fork` | Added large-map stress case: chart/probe/cubemap budgets, bake time and memory, distant visibility, offscreen shadow casters and movement between areas. |
| `d1_trainstation_02` | Added campaign-map case: fixed and moving views, indoor/outdoor lighting transitions, props, reflections and bake/cache behavior. |
| Large combined Half-Life addon maps | Added long connected-layout stress case: room/portal transitions, probe and cubemap boundaries, cumulative bake work, memory and traversal stutter. Inventory installed BSP names first; include the `hls00amrl` authored-reference comparison where available, then select additional combined maps. |

Bake input readiness now follows retained brush/overlay/terrain/prop material dependencies rather
than the entire BSP material inventory. Required unresolved inputs still block publication. The
Trainstation acceptance gate remains open: its retained `models/props_combine/com_shield001a` uses
Source `Refract` with no base texture, normal/tint maps and animation proxies. Native baking now
supports explicitly measured finite dielectric sheets: two-interface reflection/transmission,
textured absorption and containing-volume classification. CPU ownership/validation and hardware
transport tests cover both sides, distance independence and sheets inside a volume. Native live
sheet refraction now shares the late MSAA pass, with finite thickness, textured absorption, an exit
offset, visible-sky transmission and shared SSR/cubemap/hardware reflections. It assumes air outside
the primary sheet; camera-media/nested-layer integration, full path-tracer parity and adapter
material/proxy integration remain pending. The testing checkpoint includes the native paths but does not unblock Trainstation
yet. Source's artistic refraction amount is not a physical IOR. Define a stable optical tint policy
explicitly: the tint sampler can share animated
bump UVs. Do not freeze an arbitrary animation frame, skip the dependency or mark a placeholder ready.
Check complete first-load sky capture and radiance separately once this gate opens.

Vulkan path-tracer shadow visibility now has descriptor-indexed measured-sheet data and shares
the finite, angle-dependent transmission model. Surface and frame GPU record sizes are preserved;
dry scenes allocate no optics buffer. Primary/continuation shading now shares finite sheet optics,
with specular-only camera composition, compensated secondary transport and SHARC updates that
exclude camera attenuation from cached outgoing radiance. Ray/raster raw camera, stacked-sheet
and populated-cache transport cases are covered. Textured primary optics, roughness, denoised
guides, nested camera media, Metal optical-buffer integration and full backend/material acceptance
remain pending. Keep the blended-secondary-ray policy explicit
in comparisons so disabling those rays cannot masquerade as a transmission error.

Transport distinguishes dielectric visibility from containing-volume classification. Scenes with
only finite sheets skip the six receiver-containment directions; scenes with bodies retain them,
including traversal past sheets to find a body's exit. The same transport descriptors serve the
optional live GI and hardware reflection paths. This removes redundant classification work without
disabling sheet transmission or changing the public material layout.

The combined addon supplies `hls00amrl`, `hls05amrl` and later chapter BSPs for concrete follow-up
selection. The exact `gm_fork` must still be located or made available; `rp_fork_stasiland` is a distinct
map and does not fulfill that matrix entry.

For each selected map, retain cold-bake and warm-cache checks, fixed-camera timing windows and an
armed traversal. Record effective quality/settings, focus, GPU clocks, scene identity and content
mounts with local results. Separate bake latency, completed-bake frame time, host waits and GPU pass
costs; include visual checks of lightmaps, dynamic probes, reflections and shadow continuity. Keep
large-map memory and bake budgets bounded rather than reducing quality silently to complete a run.

## Handoff checkpoint (2026-10-20)

The handoff prepared this branch for another contributor without claiming renderer completion.
Read [the continuation guide](forward-plus-handoff.md) first. The feature list below includes historical
checkpoints; an implemented feature is not automatically accepted in live gameplay.

At the handoff, native `e51bff0` and adapter `712a3aa` remained installed, and the adaptive work
was an unreleased research checkpoint. The original recommendation to keep that install fixed has
been superseded by the reviewed 2026-10-22 testing checkpoint above. Fixed-view improvements and
component checks do not close live-game performance, broad probe quality, moving-viewmodel or
other-map acceptance. Continue those gates rather than treating testing deployment as release completion.

### Consolidated requested work and remaining acceptance

| User requirement / report | State and remaining work |
| --- | --- |
| Source-2-style cheaper backend; clean, modular readable code | Core Forward+ backend implemented. Review the new adaptive modules and lifetime/cancellation boundaries before release; no promise of live FPS parity with path tracing. |
| Fully baked direct + indirect on static surfaces and probes | Canonical baked runtime implemented, with attributed corrections, selected highlights and conservative unknown fallback. Finish live map-scale correctness/performance acceptance and retire only proven-redundant paths. |
| Scalable single-channel visibility/index layers, sharing nonoverlapping lights | Indexed R8/shared-page/compressed encodings and attributed directional fields implemented. Validate arbitrary overlap, budgets, cancellation, cache migration and unknown coverage; do not claim a confirmed CS2 four-light or 1024-unit rule. |
| Toggleable/named/styled lights baked and independently controlled | Grouped direct/indirect data implemented with scalar runtime control. Broaden live toggle/removal/replacement and mixed-shadow checks; native contracts must remain engine agnostic. |
| Surface atlases using a fixed memory/texture budget | Surface charts, world-space density and bounded page packing implemented. Quality varies. Fit density from the target budget rather than an arbitrary user luxel spacing. Inspect chart continuity, seams, page utilization, skybox weighting and backface waste; verify practical 2K/4K page caps within memory limits. Pages are intentional; one 4K atlas is a request to evaluate, not a requirement to discard paging. User later preferred 4K/compression over multi-GB allocations. |
| Soft radial emitters, soft static shadows and antialiased direct bake | Finite emitter transport and sunlight refinement exist. Improve local/overflow and thin-shadow refinement, indirect noise and connected seams; compare path-traced emitter semantics. |
| Construct artifact reproduction | Target the two-ceiling-light room behind the usual spawn, its bent connector and the secret room, as identified by the user. Compare neutral baked irradiance with ordinary shading before attributing artifacts to atlas packing. The default chart rectangles occupy about 96.8% of the four-million-texel allocation in the current replay. A padded-chart denoising experiment did not affect production's zero-padding layout and was removed. Replay now supports static LDR sky faces and frozen live sky references for procedural skies; use the latter for Construct rather than accepting the old flat-background bake as an in-game match. |
| Denoiser chart-edge colour fringes | Reproduced blue/purple secret-room strips and connector bands in neutral irradiance with a frozen live sky. Temporary image alignment now continues each chart's edge data instead of injecting black HDR / minus-one signed SH padding; reproduced bands disappear without increasing atlas budget. CPU denoising, cache, atlas and GPU compatibility checks pass. Filtered cache identity changes; raw-bake identity remains unchanged. The testing install carries the fix, and ordinary-material replay with full SSR and auto exposure retains the repaired edges. Wider-map and temporal-exposure acceptance remain open. |
| Construct ceiling-fixture halo | Persists at eight million surface texels, so retain the default four-million budget. Fixed-camera, fixed-sky, fixed-exposure path tracing with thousands of accumulated frames does not show the bright halo. Earlier neutral/coverage captures included an unintended opaque reflection overlay from late water composition; those captures cannot establish pure baked-light attribution. With water disabled, neutral irradiance still retains the halo and coverage separates the ceiling lightmap from lamp probes. The source ceiling normal is correctly downward. Surface-hemisphere transport fixes an isolated self-lighting defect but does not remove this halo. A verified 256-versus-1024 material-input-atlas comparison also retains the halo, so do not increase that budget for this artifact. Isolate per-component light contributions and material/emission-mask semantics next; other Construct artifacts remain open. |
| Lighting diagnostic reflection isolation | Late water composition previously added opaque reflections to diagnostic views even when the water itself was occluded. Water composition and exposure now honor the same diagnostic exclusion as the final compositor. The SSR/MSAA regression compares occluded-water on/off for neutral and coverage views; ordinary water and exposure tests remain required. This corrects diagnostic evidence, not the remaining Construct fixture halo. |
| Surface back-hemisphere self-lighting | Surface receivers previously projected the whole sphere into low-order directional SH. A coplanar emissive-patch regression reproduced nonphysical front irradiance. Surface paths now sample the incident hemisphere with the same ray count; aggregate direct samples also reject rear directions. Zero-normal volume probes retain full-sphere lighting. Serial/parallel transport checks cover constant sky energy, directional response, retained front direct light and volume signal. Full transport validation passes. Shader bytes automatically change bake identity; no extra atlas memory is allocated. Broader map acceptance remains open. |
| Path-traced reference history and replay readiness | Unconditional wall-clock animation time invalidated scene history even without scrolling materials: actual replay reported one accumulated frame after 269 traced frames. The clock now advances only for active scrolling normal layers and freezes at time zero during progressive accumulation. A longer Construct reference retains one scene generation through thousands of accumulated frames. CPU layer-selection and focused live-water/progressive-history regression checks cover the policy. Backend/denoiser equality alone still does not prove all shader preparation finished; explicit replay preparation readiness remains to do. Validation-enabled GPU runs also report semaphore-state timeouts; resolve or characterize those before claiming clean validation. |
| Hardware-accelerated, fast map-load baking; OIDN like vrad3 | HW ray-query transport, budgeting, indexed visibility, bounded OIDN calibration/CPU queues implemented. Cold-map work/latency still need broader profiling. Do not mislabel GPU frame time or whole startup time as bake transport time. |
| Long overflow visibility stages; c1a0d noise; Lost Coast/Steamlab baking | Some bottlenecks and chart faults fixed. Keep d2_lostcoast, dm_steamlab and c1a0d on the regression matrix; these reports are not universally closed. Intersecting lights and range/unit conversions need transport parity review. |
| Static world excluded from mixed moving-shadow maps | Implemented caster membership and empty-map correction shortcuts. Verify terrain, static props, replacements, map transitions and viewmodels; keep offscreen dynamic casters. Leave engine frustum culling off as requested. |
| Mixed shadow fade, point-light filtering, wall leaks | Timed selection/highlight fades and filtering changes exist. User suggested about two seconds; verify live transitions, penumbrae and c1a0 close-wall pre-bake artifacts. Construct white-room toggleable lights are an important leak reproduction. Audit whether the ray-occluder fallback actually fixes specular leaks; retain it only with demonstrated utility. |
| Stable persistent caches; no rebake on camera/weapon changes | Cache validation and invalidation receipts exist. Investigate remaining spontaneous/weapon-change rebakes; distinguish real light/material edits from identity bugs. Warm replay parity is not proof that installed live caches always work. |
| Auto exposure and same tonemap path as path tracing | Shared postprocessing and exposure controls implemented. Refresh live adaptation, HDR/water and mode-switch acceptance; coverage diagnostics intentionally bypass automatic exposure. |
| Automatic tolerant parallax bounds, overlap/feathering, priorities | Default reflection budget is 64. Current normal-quality automatic comparisons publish 63 captures on Construct and 64 on Crossfire; authored overrides replace that set. Capacity improved hall coverage; automatic room fitting, bevel tolerance, doorway/wall separation, rotations and blending remain open. Capture point and influence/projection boxes are distinct. |
| Better dynamic probe quality in Construct's main room | Target is the diagonal roof-slit skylit hall, especially props. The adaptive field, cache transitions and shared nodes are available in the testing checkpoint but still need broad visual acceptance. Per-record light-volume support fixes a budget/fallback cost issue; mixed receiver quality and current performance against the earlier uniform checkpoint still require acceptance. Validate moving props and other maps, not just six spheres. |
| Cubemaps captured after lighting | Ordering is implemented; verify invalidation/re-capture after styled/material/lighting changes without unrelated full diffuse rebakes. |
| Roughness-aware s&box-like SSR, full resolution default, viewmodel SSR | Implemented paths and tests exist. Refresh live roughness, disocclusion, blend with cubemaps/RT, and actual weapon acceptance. |
| PBR/AutoPBR and shared Remix replacements | Shared asset/material path implemented, including safer Source roughness fallback. Verify late replacement arrival, normals, BRDF/roughness parity and authored PBR examples; no separate incompatible asset conversion path. |
| Viewmodel overlay, world probe lighting, received/self shadows | Dedicated close-up path/cascade, overlay and isolated self shadows implemented. Verify weapon changes, world-shadow reception, offscreen casters, SSR and that self shadows never affect the world. |
| Decals borrow underlying lighting | Receiver borrowing implemented. Review fullbright exceptions, modulating decals, close receiver selection and live overlays. |
| Water vaguely compatible with path tracing; CS2 water later | Optical-material, normals, HDR refraction, SSR/RT and underwater paths implemented. Broader lifecycle/noise/material parity remains; exact CS2-water recreation is a later aspiration. |
| F12 cursor capture/menu usability and sky pitch/flip | Input bridge and sky changes have checks, but physical mouse/engine acceptance and reported sky behavior need live retesting. Separate pre-render engine startup exits from renderer defects. |
| Performance: Crossfire, c1a0, Construct white room, stutter | Clustered screen tiles/depth slices already exist. Profile CPU submission, fragment invocations/overdraw, shading, lists, shadows, presentation and spikes separately. Replay is not live-game FPS. Do not repeat rejected shader/storage/draw trials without new evidence. |
| Optional screen-space shadows, virtual shadow maps, distance reflections | Research/backlog only; retain present techniques as defaults. Do not expand this before correctness/performance release gates. |
| Cubemap editor with textured worklight viewport, gizmos and right-mouse WASD | Native Qt workbench exists in adapter tools, with face handles, move/rotate/independent capture/priority, undo/save and live sidecar publication. Polish/test against BSPs; new standalone tooling should prefer C#. Larger Hammer-2-like replacement editor is a later project. Do not redistribute Valve icons/styles. |
| Disk cleanup, testing install, official remotes and upstream merges | Testing install remains separate; official remotes/feature branches retained and main untouched. Preserve user configs/addon mounts and authored data. Cleanup needs verified payload receipts; refresh upstream merges deliberately, then retest before deployment. |

Reference engines are Counter-Strike 2, Half-Life: Alyx and s&box. VRF's Renderer is an unofficial
Source 2 resource viewer/reconstruction, useful for format/shader interpretation but not proof of Valve's
compiler behavior. Study extracted lightmap/probe assets and authored combined volumes; avoid inventing
engine limits from filenames, channel count or a single map. A future exact Hammer-style editor is not
a prerequisite for the renderer handoff.

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

Optimize cluster construction as well as receiver shading. In clustered baked-tail mode,
`cluster.comp` still tests the full uploaded light list for every depth-sliced tile; selected
highlight counts do not describe that work. Cooperative 64-light batches now share sphere-bound
loads across each workgroup without changing membership; spatial candidate reduction remains open. Evaluate conservative spatial broad phases or reuse
with exact input invalidation, while preserving unknown-coverage fallback and per-receiver list
overflow. Any reuse must follow actual GPU submission/resource lifetime, including abandoned
recordings; recording commands alone does not prove their output exists. Compare moving-camera
cases as well as stationary views and retain whole-frame timing alongside nested pass ranges.
Treat depth-only rejection as an experiment rather than an assumed win: a large-radius light
can span most depth slices, and additional scalar branches can duplicate the existing sphere/AABB
test. Require repeatable whole-frame improvement before retaining new pruning metadata. Keep
near/far tangencies, both depth signs, mixed-prefix ownership and overflow in GPU regression coverage.
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
| Automatic parallax cubemap bounds with tolerance for room details | Coverage-based selection and default 64-capture capacity; receiver bounds contact nearby axis-aligned room surfaces independently of inset projection boxes | Limited budgets still leave rooms uncovered; review transitions, rotated rooms and reflection leaks across multiple maps |
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

The two authored Construct volumes now provide separate capture-placement and receiver-domain
fixtures. At the smaller room's authored capture position, fitting generates a room-shaped
alternative but orders a larger corridor first. The nearby automatic doorway position does not
generate that room-shaped alternative. These are separate failures: changing the final selector
cannot recover a shape that placement never generated. Brush-surface sampling confirms that a
visible-surface objective can prefer the room alternative without consulting authored bounds;
this is an offline lead, not an accepted runtime heuristic. The opt-in candidate report now includes
bounded, area-weighted samples from frozen surface charts, independent of atlas resolution. Next
compare marginal visible surface coverage and projection error using these actual bake receivers, and
test both automatic origins and authored origins. Preserve wall separation, independent influence
and projection bounds, capture/ray budgets and cancellation. Include terrain, props and material
visibility before accepting the result; two partial authored rooms are not whole-map ground truth.
Do not replace fitting with authored-volume union, loosen occupancy globally or add runtime
receiver rays per fragment on the strength of this subset study.

The complete Construct site pipeline also finds automatic interior origins that generate room fits;
the earlier doorway-only comparison did not describe the whole candidate set. Primary-origin records
can consume the old shape-audit allowance before those alternatives are assessed. All retained fits
now reach assessment, with independent limits of 160 unique origins and 480 records and shared
first-hit rays. Final capture limits and volume-based selection remain unchanged. This improves the
observed Construct/Crossfire receiver assignment, but gains coexist with losses elsewhere; validate
actual room transitions, wall leakage, moving views and frame cost before deploying this checkpoint.
Automatic same-origin shapes now share lighting queries and reuse bit-identical filtered mip chains,
while retaining independent projection/blending records. Cold/warm cache reconstruction uses the
same unique query plan. This removes duplicate capture/filter work but keeps texture storage per
volume. Default-quality fixed-view comparisons at 1080p, 4x MSAA and full-resolution SSR
show no large steady-state GPU regression on Construct or Crossfire. Construct's small difference
is below baseline repeat variation; Crossfire improves in the sampled view. Cached versus fresh
lighting and fixed cameras limit attribution and do not establish gameplay performance acceptance.
Moving-view, room-transition and wall-leakage acceptance remain open beyond the testing checkpoint; shared
texture storage remains a further optimization opportunity. Authored edits now reuse previously published radiance by exact capture-ray identity,
including when a volume gets a new ID or another volume uses that same origin. Signed-zero origins
remain distinct; changed frozen scenes or lighting configuration invalidate the authored state.
New same-origin volumes in one edit also share capture queries and filtering while retaining
independent projection metadata and priorities. GPU publication still allocates one image layer per volume.
The adapter replay now supports bounded, frame-indexed camera paths with numbered images and a
position trace, separate from fixed-view timing. Installed-baseline and development runs complete a
short main-hall path with unchanged active bakes. Use this to inspect actual boundary crossings;
successful capture and geometric influence coverage alone do not close visual continuity or leakage.
A matched SSR-disabled camera route through Construct's adjoining stairwell also completes without
rebaking. Dark existing materials provide only weak evidence about reflection transitions; use clear
reflected features across overlaps before accepting parallax/blending, including the lower-basement
coverage tradeoffs. Preserve SSR-on and ordinary-exposure integration checks.

Cubemap sharpness remains below the reference target. The development default is now 128-square faces;
the s&box reference exposes 128/256/512/1024 and defaults to 128. Earlier matched captures at
32/64/128 show clearer reflected detail at higher resolution. The former RGBA32F buffer payload
also reduced capture capacity at 128-square faces on minimum-range devices. Production now uses
compact projection metadata plus RGBA16F image layers, with six vertically stacked faces per layer.
The image payload is half the previous radiance size, avoids optional cube-array features, and
retains 64 captures within core image-layer limits. Canonical CPU/cache data stays full precision.
Bake workers and authored-edit workers prepare immutable image owners; the render queue uploads
and publishes them, and frame fences retain uploads and sampled owners. Reflection composition,
viewmodels and water share the single image-backed sampler. The obsolete buffer-radiance shader
path is removed. Settings and UI retain smaller sizes and expose 256/512 quality choices.
The face-size control estimates image texel memory at the selected capture budget.

CPU, image upload/readback and sampling tests cover HDR precision, all faces/mips, cancellation,
empty fallback, 64-capture membership and 256/512-square faces. Cache validation separates image
limits from metadata range; a regression reads a payload larger than its device buffer range.
Authored-edit, viewmodel environment/SSR/shadow, transport and asynchronous-target integration
checks pass with synchronization validation. A small-budget Construct replay also completes with
live 256-square image captures. A small-budget 128-square Construct cold/warm pair reports a
cache hit with identical published captures and lighting hashes. These checks do not establish
full map visual acceptance. Normal-quality repeated fixed views retain 63/64 captures on
Construct/Crossfire at both 32 and 128. Crossfire shows similar steady GPU cost; Construct has
larger run variation than the average resolution difference. This supports the sharper branch
default without establishing gameplay performance or a general speedup. Check wider room coverage,
mirror/rough materials and normal-quality cache transitions for broader acceptance. The separate testing
install now carries the image-backed 128-default checkpoint and bounded CPU filter workers; settings,
addon mounts and the two authored Construct volumes are preserved. This is not full quality acceptance.
Authored sets currently replace the automatic set entirely. Partial authored maps therefore need an
explicit overlay/fallback design that respects priorities, the 64-record budget and empty-set semantics;
do not assume automatic coverage survives outside the two reference volumes.

Expanded tests exposed a three-face corner discontinuity in the previous manual filter. CPU mip
preparation and the shared shader now average the diagonal corner tap across the three meeting
faces. All eight corner approaches have an explicit regression test. Policy 28 introduced this fix;
current cache policy 29 also prevents reuse of sets truncated by the old buffer-capacity policy.
Regenerated shaders must travel with the resource/descriptor changes. s&box's square-root roughness
lookup belongs to its different coefficient-prefiltered mip chain; do not copy the lookup without
matching the producer. Disabling the independent highlight budget still retains mixed-light
specular work, so it alone does not isolate cubemap radiance. Further work includes shared GPU
image storage for same-origin shapes, streamed high-resolution capture rays, compressed cache/GPU
formats, GPU GGX filtering and calibration against all three reference renderers. CPU GGX filtering
now distributes independent mip rows through the existing bounded worker pool in automatic bakes
and authored edits. Per-texel sample order and duplicate-capture reuse stay unchanged; packed serial
and parallel results match byte for byte. This targets CPU finalization, not steady rendering cost.
GPU filtering and streamed capture work remain outstanding. Higher face sizes
also increase canonical raw/filtered cache size and CPU capture/filter work; compact GPU images
alone do not solve those bake-time and disk costs.
The attempted receiver-first score is rejected: it can fill the sheltered-region budget and displace
broad map coverage. Its runtime code is removed. Develop room-local fairness before revisiting that
objective; do not equate global area sampling or additional audit records with complete quality.

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

Construct sky-prop clipping now removes the reproduced white-room tree intrusion
without disabling sky props. The adapter clips placed triangles, rather than only
testing model origins. Fresh/warm bake parity and unchanged Crossfire cache/output
are verified; two edge pixels, full replacement meshes and live acceptance remain
open. See sky-prop-clipping-2026-10-20.txt.

Denoiser queue calibration now measures parallel CPU independently of serial
CPU selection. Both measured margins remain required. Normal Construct controls
show unchanged bake time; a validation diagnostic exercises the formerly excluded
choice. Existing cache restoration is exact. See denoise-queue-selection-2026-10-20.txt.

Sparse styled direct/indirect merging now skips tiles proven black in both
validated input maps. Construct grouped preparation saves about227ms in paired
warm controls, with exact validation screenshot/cache restoration. Cold-map and
live performance gates remain open; see sparse-empty-tiles-2026-10-20.txt.

Grouped direct composition now marks conservative per-style chart tile occupancy,
including full captured rectangles/seam donors. Construct paired composition mean
559->391ms; validation cache/image remains exact. See direct-style-occupancy-2026-10-20.txt.

Exact offline BSP half-space intersections confirm that conservative clipping
alone does not explain the authored-volume mismatch:13/14 reference boxes still
exceed8% solid/outside partition volume. Keep runtime fitting unchanged until a
receiver-domain/wall-separation approach passes real-map coverage; see
cubemap-exact-occupancy-2026-10-20.txt.

Construct alternative-audit controls reject both spare-slot spreading and full
shape expansion:208vs144 boxes with unchanged49,536rays selects identical captures
and image; spreading regresses surface coverage. Neither runtime trial remains.
Do not repeat scheduling changes without a new fitting/receiver-quality objective;
see cubemap-alternative-budget-2026-10-20.txt.

The earlier Construct selector investigation ranked corridor candidates50/53,
beyond the then32capture cap. A two-sided wall-support score changes only1/16
selections and leaves sampled receiver coverage unchanged. Next investigate bounded
per-tile probe assignment/larger map sets or local residency versus a receiver-aware
fixed budget; do not simply enlarge the per-fragment global scan. See
cubemap-selection-capacity-2026-10-20.txt.

Workgroup-local probe candidate masks now cull the screen-space reflection
filter conservatively. Oriented/priority/roughness/partial-group tests pass; matched
Construct/Crossfire screenshots remain exact, reflection pass saves~9/~2.5us.
Other production shader stages retain original walks. That checkpoint established
the lookup primitive; the subsequent64capacity implementation is described below.
See probe-tile-culling-2026-10-20.txt.

Reflection storage, two-word masks, cache headers and authoring now support64.
Construct selects54captures including the previously omitted local origins;
Crossfire selects64. Existing16capture caches retain exact visible output.
That checkpoint retained default16; the current default budget is64 to improve
room coverage. Explicit saved budgets remain respected. Larger sets are a quality
choice, not an FPS win, and automatic room bounds remain open.
See probe-capacity64-2026-10-20.txt for limits and tests.

Irradiance records now use sparse lattice addressing when the host supplies a
closed visibility partition. Empty nodes use zero directory entries, placement
fits the physical-record budget, and CPU/GPU reconstruction, relocation, styles,
local visibility and persistent storage share the same addressing. The active
diagnostics expose actual density. Dense hosts retain their complete coverage.
This is still a single-resolution lattice: local adaptive refinement, resolution
transitions and real moving-prop comparisons against dense reference fields remain
the next probe-quality work.
