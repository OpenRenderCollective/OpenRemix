# Forward+ delivery roadmap

## Current lighting-quality focus

Quality qualification must distinguish reduced-budget smoke tests, normal-budget
controlled comparisons, and live-game appearance references. The replay quality
preset now requires sky data instead of accepting a missing-sky fallback; procedural
skies require a frozen live capture. Record completed publication, actual sky and
display transform before interpreting noise, brightness or reflection differences.
An earlier installed Forward+ functional capture omitted Construct's procedural
sky and therefore does not qualify its live lighting appearance. Later checks use
a captured live sky. Preserve valid controlled component studies, but do not
present them as complete visual acceptance. Current status in this section
supersedes older dated experimental checkpoints later in this file.

An installed Forward+/path-tracer transition smoke reached Forward+ capture but
not the final path-traced image. Two synchronization write hazards were reported
inside DLSS RR resource clears. Isolate RR on/off and backend-transition versus
fresh-start cases before deciding ownership; the combined smoke remains incomplete.


Reference coverage is a prerequisite for the quality work below. Incoming-ray
hemisphere integration omits analytic direct lighting at the receiver, while
fully baked surface and probe aggregates can include it. Such references cannot
establish total-lighting error against those aggregates. Requalify earlier
reference-based placement and reconstruction conclusions with matching lighting
components before choosing defaults. The offline `surface` reference format
retains receiver normals and uses the surface irradiance estimator, including
the aggregate direct-light policy. Keep sampling noise, spatial interpolation,
normal bias and missing lighting components separate in these comparisons.
Base-only reference requests now make lighting composition explicit: prefix a
request with `base` to compare extracted base atlas/grid data without styled
additions. Keep those comparisons separate from complete shader-output checks.
Fixed-site `surface-offsets` diagnostics now preserve receiver normals and
analytic direct lighting across repeated sample streams. Use these to screen
estimator convergence independently of atlas filtering and published probe
reconstruction. Raising the shared bake budget can also change adaptive probe
allocation and reflection work; a normal/higher-budget image pair alone does
not isolate surface noise. Keep coverage/domain fixes and convergence separate,
and qualify targeted quality allocation before increasing global defaults.


Coplanar seam donors now retain their original receiver footprint, and sunlight
edge refinement admits those verified aliases instead of skipping every
filtering-only donor. Curved donors without a proven footprint remain excluded.
Sunlight coverage changed in cache revision 34; current candidate revision 36 also
constrains relocation and refinement binning to the receiver side of solids. This corrects missing
edge antialiasing, not probe leakage or chart-local indirect filtering bias.
Continue checking sunlit boundaries and indirect discontinuities separately.
Refinement decisions are shared across visible copies of a verified footprint:
an edge detected in one chart also refines its source and other chart copies.
This prevents a chart-local edge search from leaving one copy unrefined.

Generated charts can join exact position/normal duplicates across material UV
seams. Material texture coordinates stay intact, while hard normal seams and
nearby unmatched vertices remain separate. This reduces avoidable chart splits;
broader filtering-cost and quality qualification is still required.

Generated model-chart density is also a separate confirmed failure. Grazing
projection faces and excessive curved metric expansion now split candidate
patches rather than inflating their global atlas footprint. Whole-map CPU atlas
preflight is available to study authored and generated charts together without
running lighting transport. The opt-in runtime experiment now derives its chart
limit from minimum padded atlas capacity, with an absolute 1,048,576-chart ceiling,
instead of rejecting larger maps at 65,536 charts. Actual packing still must fit.
Runtime profiles now enable eligible static mesh lightmaps by default through
`rtx.native.forward.lightmapModels`; low-level bake callers retain explicit control.
The existing atlas budget is shared with world surfaces, and completed policy-7
model caches remain compatible. Reduce chart count and filtering work without
sacrificing coverage or density. Broader map/replacement quality and memory-cost
qualification remains open; dynamic objects still need the volume fixes below.

Model-chart preparation, full lighting publication and disk reload have now been
exercised on Construct, Crossfire, c1a0 and bm_c1a1b at the normal sample budget.
Runtime-default cache reuse without developer overrides is qualified on those
maps. A private live Construct check also preserves baked content through camera
changes, physics-prop creation/removal and weapon switching, with static shadow
updates remaining zero. Continue visible seam/noise, moving-receiver, replacement,
chart-heavy filtering and wider-map work; default promotion does not close those
quality/performance gates. Constant-field filter bypass and bounded per-chart
queues already exist; do not reduce sample counts or merge unrelated chart images
merely to make qualification faster.

Construct's back-spawn ceiling-fixture housing has a confirmed probe-lit diffuse
error. Total diffuse (`debugView=7`) retains the excess without emission/specular;
coverage distinguishes the housing from its lightmapped ceiling. The bounded
receiver-ray report reconstructs the neutral pixels and exposes bright probes
connected through the roof slab, plus connections through the housing itself.
Keep world-atlas seams, emitter shape and prop/probe lighting as separate cases.
The receiver-ray report now audits each nonzero stencil connection against
rigid opaque geometry, retaining the exact biased coarse/fine endpoints, blocker
ownership and normalized blocked-weight share. Missing eligible blockers do not
establish alpha/transmission visibility. The audit is optional CPU work, with
at most sixteen connections per receiver; reconstruction weights stay unchanged.

Construct confirms both roof-side support leaking through opaque slabs and a
normal-biased query crossing into a nearby ceiling even while the actual receiver
is below it. Keep bias/domain containment distinct from estimator variance.
Some queries have no unblocked stencil support; rejection-only fixes can turn
those pixels black. Next study receiver-side support and bias containment together
without adding per-pixel geometry queries to the normal rendering path.
A frozen-field study rejected globally reducing the normal bias to half, quarter
or zero: identical 8192-site/surface-reference cohorts retained coverage but had
worse mean and RMS errors, with severe local regressions. The temporary override
was removed. Next test selective containment against local geometry while
leaving valid bias offsets intact; do not treat zero bias as a general leak fix.
The reports now retain a bounded closed-partition containment oracle alongside
the published estimate. A near-to-far segment walk detects thin solid/unplayable
intervals even with playable endpoints; invalid, open or budget-exhausted data
remain unknown. It preserves clear bias offsets and caps a private coarse bias
when a crossing is found; fine queries inherit that conservative cap. Grid
copies are limited to 65536 records. No shading or cache data is modified.

The world triangle-centre cohort rarely exercises this defect. Camera-picked
housing points do: partition containment restores some clear connections in
the all-blocked case, but substantial blocked weight remains and other points
can regress. The host partition omits model detail, so this is not a full opaque-
geometry containment test. Next compare that fuller geometry oracle and
receiver-side support/domain reconstruction before designing cheap GPU metadata.
The camera diagnostic now also limits bias against eligible rigid opaque model
geometry, and reports exact tested-connection SH reweighting alongside the
original estimate. Endpoints exclude 0.1 mm; absent support is null, not black.
These are optional CPU oracles, not additional queries in normal shading.

Matched high-sample hemisphere references at selected housing pixels confirm
bias clipping alone is insufficient. Clipping plus connection rejection is
substantially closer there and restores a site lost by rejection alone, but
still underlights on average. Do not promote either unilateral shortcut. Next
study reusable geometric support/domain data, including compact-object support
caches, against these fixed references and wider maps; require coverage, movement
stability and measured cost before choosing a runtime representation.
The synthetic painted-sky fallback does not qualify live-game sky parity.

Independent 8192-sample SH captures 10-25 cm outside three housing faces are within
about 0-10% luminance of surface hemisphere references; the published interpolated
field is roughly 2-3 times too bright. A 50 cm offset makes one site almost black.
Prioritize geometry-aware receiver-adjacent support within the fixed record budget,
including intervening surfaces and thin geometry. Angular SH order alone is not
the primary error at these sites, and arbitrary outward relocation is unsafe.

There is also stochastic probe error: matched 256-sample SH differs from the
8192-sample result by roughly -40% to +38% at several 10-25 cm sites. This is one
deterministic sequence, not a multi-seed variance estimate. OIDN filters surface
lightmaps, not volume SH. Qualify probe convergence separately from placement;
do not globally increase surface sample budgets or smooth coefficients across walls.

`rtx.native.forward.bakeProbeSamples` now provides a separate volume-transport
budget (0 inherits `bakeSamples`, explicit values up to 8192). It covers base
and styled probe coefficients, not surface transport, reflections or shadow
visibility. Adaptive placement retains the original sample budget, followed by
a separate final-quality pass when needed; raising coefficient quality must
not silently move the allocator to different cells. Equal effective sample
counts preserve existing cache identity. The default remains inherited.

Repeated fixed-site captures show better convergence at 1024 than 256 samples,
but published-field gains are modest and local regressions remain. Do not
confuse lower estimator variance with solved side/domain ownership. A direct
quality override during refinement was rejected because it changed allocation
and produced severe new outliers. Continue geometry-aware field work and wider
map checks before selecting a higher default.

Rejected shortcuts include blindly dropping blocked contributors, low-variance
all-axis depth clamps and a single stratified indirect-light dimension. They trade
leaks for dark neighboring faces or show no overall improvement. The earlier
directional-depth option remains off because barrel/crate faces regressed.
Stratifying only aggregate direct-light selection leaves this fixture unchanged;
its explicit lights remain mixed, with no selected aggregate lights here.

Indirect-light proposal resampling was removed after a repeated-stream screen.
Its first 256-sample housing sequence looked better, but four offsets at six fixed
sites increased relative luminance RMSE from 32% to 37% and maximum error from 56%
to 121%, against 8192-sample SH at those same sites. Mean absolute error improved
only from 29% to 25%. Passing analytic energy, component-additivity and cache tests
did not establish useful scene convergence. Do not restore the candidate from the
single-sequence result; no runtime switch or unused reservoir path remains.

The bounded offline reference tool retains `sh-offsets <sample-count>` input,
followed by XYZ/first-sample rows (at most 16 sites, 4 KiB). This tests repeated
streams at identical positions without geometry jitter. Output attributes each
sample offset to its site; default SH and hemisphere formats remain unchanged.
Four offsets are a rejection screen, not independent-seed confidence intervals;
the high-sample SH reference also retains uncertainty.

A shorter-clearance relocation fallback was also rejected as a standalone fix.
At 2 m spacing it can recover a valid near-ceiling slot, and independent SH there
matches the housing references. The normal-budget housing field remains at 4 m,
however, and its measured stencil is unchanged. Extra recovery also changes coarse
scores and reallocates refinement elsewhere: an 8192-receiver surface-reference
audit shows almost unchanged average/RMS error but severe new bright outliers.
Treat near-surface recovery, adaptive priorities and fixed-budget allocation
together. The current scorer uses lightmapped receiver normals; uncharted static
model receivers such as the housing need explicit consideration in that design.

The native `modelReceivers` sampler now supplies bounded uncharted rigid-opaque
triangle centres, with inverse-transpose normals and up to six representative
directions per draw. It excludes charted ranges, overlays, skinning, uncertain
alpha/transmission and centres inside closed-partition solid/unplayable leaves.
Budget rounds span eligible draws before taking more directions from one model.
`REMIX_DEBUG_BAKE_MODEL_RECEIVERS` writes a fresh geometry/current-base-estimate
manifest on publication or cache load; this is not an independent light reference.

Matched unstyled hemisphere references at the sampled housing orientations expose
large errors that the lightmapped-only scorer misses. A bounded GPU-reference
prototype successfully promotes the housing region to 2 m within 32768 records,
but it was removed: the resulting estimates are worse, including with the existing
directional-depth option. Solid nodes near the roof recover toward the bright
rooftop side, and their interpolation support extends below the slab. Better
priority scores and finer spacing alone do not provide side-aware ownership.
Do not repeat that standalone allocation change or leave its extra capture stage
in default bakes. The existing allocation and cache policy are restored.

The static-model surface-bake candidate is available through
`REMIX_DEBUG_BAKE_MODEL_CHARTS=1`, disabled by default, with private derived charts
and a separate conditional cache key (policy 6). Shared range validation preserves
caller-authored charts, retained draw bindings and the existing world-metric atlas
budget. Bounded connected consecutive patches reject projected overlap, opposing
faces and invalid curved mappings. No private index-reordering path remains.

The housing's bright triangular patches were missing surface coverage, not proven
chart seams: its brightest checked pixels hit uncharted triangles 177, 213 and 231,
which fell back to leaking probes. Their centres were inside solid BSP space while
visible portions reached the room. Whole-triangle centroid rejection is replaced
by conservative fragment traversal. Fully buried faces are excluded; partial
triangles, missing/invalid topology and exhausted traversal budgets retain coverage.
Tests include a playable fragment with solid centroid and all three corners,
cancellation, fully buried faces and conservative traversal fallback.

Normal Construct now generates 1380 model charts and 3792897 surface texels in
four pages at 0.248 m spacing (previous candidate: 0.241 m). All three target faces
are charted and the checked bright patches disappear in neutral indirect and
shaded views. A cache reload is pixel-identical at the target camera, with zero
grid/surface bake batches. Five neutral and five shaded cameras complete without
validation errors; existing secret-room and probe-quality issues remain. These
are frozen replay checks using synthetic sky, not live-game sky parity or default
qualification. No deployment change has been made.

Crossfire also completes a normal 256-sample/three-bounce policy-5 bake with 223
model charts, 3614696 surface texels, four pages and 0.0831 m spacing. Three
stationary rooftop orientations were inspected in neutral/shaded modes; cache
reloads record zero grid/surface batches and no validation errors. This is a
functional/visual screen, not a fixed-reference accuracy or foreground-FPS gate.

bm_c1a1b blocks promotion: the same normal-quality candidate exceeds the 65536
model-chart safety cap during preparation, before publishing lighting. Dense-map
model chart/representation cost must be measured and addressed with better
bounded grouping/packing or genuinely baked vertex lighting under shared budgets.
Do not simply raise the cap, silently drop models onto the leaking probe field,
or introduce per-light static work. The existing default path is unaffected;
this failure does not establish its bake behavior on the map. No deployment change.

The dense-map cap now reports its stopped draw/index and the eight largest
contributors in the processed prefix; full exceptions are retained on stderr
because public status text is bounded. bm_c1a1b reaches 65536 charts across 757
contributing draws before stopping at draw 1628/index 2466. The largest prefix
consumer has 1232 charts for 4334 accepted triangles; another has 1080 charts for
5280 triangles. These are partial traversal counts, not whole-map totals. This
points to widespread chart fragmentation rather than a single exceptional draw.
The cap and default behavior are unchanged.

Next evaluate a connected mesh unwrapper against genuinely baked vertex lighting,
measuring chart count, preparation time, memory and lighting fidelity on actual
dense models. [xatlas](https://github.com/jpcy/xatlas) is an investigation candidate;
its [mesh output API](https://github.com/jpcy/xatlas/blob/master/source/xatlas/xatlas.h)
exposes chart face lists and original-vertex references, which could support
attribute-preserving private bake geometry. No dependency has been added and no
quality/performance benefit is established. Preserve authored chart ranges,
retained draw identity, controllable lights and cancellation while evaluating it.

A tested bounded OBJ diagnostic is available on chart-cap failure through
`REMIX_DEBUG_BAKE_MODEL_CHART_MESH`: the largest prefix consumer uses that fresh
path; up to seven others append `.draw-<index>.obj`. Each export is limited to
32768 triangles and preserves signed index ranges, source vertex sharing, winding,
material UVs and transformed normals. Positions are metres relative to the first
corner, with source-index/origin metadata. Cancellation and overwrite guards are
covered. No mesh is exported by default and no bake budget changes.

An offline xatlas screen pinned to f700c7790aaa030e794b52ba7791a05c085faf0c
compares those eight prefix draws: current charts 8233, library defaults 6070,
zero seam penalties 4961. Output input-corner references remain valid; this does
not establish seam/lighting quality. The largest draw has 219 exact-position
edge-connected components (770 using original indices), so dense small parts are
part of the problem. Three default runs on that indexed 4334-triangle mesh produce
910 charts in about 57-59 ms, mostly packing. These are CPU screens on a selected
prefix including repeated instances, not whole-map or foreground-FPS results.
One full draw export includes faces excluded by runtime receiver tests, so its
chart comparison is not exact for the runtime subset. No runtime dependency was
added. Next prototype parameterization separately from shared-budget packing and
compare a genuinely baked vertex representation, with fidelity and total-cost gates.

The [offline model-chart study](model-chart-study.md) CPU target validates complete imported
face ownership, input and final UV mappings, native world-metric atlas fitting and
fully mapped mesh output without a library/runtime dependency. Its synthetic smoke
test is registered; real captured datasets remain separate offline evidence. CLI
input is bounded `MCHART1` data, with optional `--baseline` to regenerate current
consecutive charts on the same original indexed mesh. No model-bake default changes.

For the indexed 4334-triangle dense model at 0.248 m manual spacing in exported
local metric coordinates: current charts use 2276 bake points/6944 rectangle
texels; default imported charts use 1805/5531; zero-seam-penalty imports use
1483/4066. Input and final mapping validation and complete mesh coverage pass.
This is not whole-map/global-phase, lighting fidelity or controllable-light proof.
An exact position/normal vertex screen counts 5044 records, so a vertex path cannot
be assumed cheaper in trace work or storage without matched-quality measurements.

Shrinking xatlas temporary packing from approximately 512 to 64 nominal resolution
only reduces the largest default case's packing from about 54 ms to 46 ms. The
current public API still materializes output via packing. Next evaluate batching
model parameterization/output to amortize this work, then qualify whole-map shared
budget fit and lighting before adding a dependency or replacing the current path.

An eight-mesh batched offline pass keeps 6070 default charts and exact per-mesh
input-corner references. Three runs take 352-364 ms total; packing remains about
337-347 ms, so batching helps modestly rather than removing the cost. Native
validation/fitting of the 32552-triangle composite study geometry passes at
0.248 m: imported charts use 12175 bake points/33279 rectangle texels versus
17730/45455 for current charts. Both allocate three 128-size pages. This uses one
composite study draw with approximate original metric positions; it does not
qualify material/draw identity, BSP exclusions, authored gaps, controllable-light
semantics, lighting quality or whole-map capacity. Next is whole-map CPU budget
preflight and a bounded in-engine candidate, not default promotion. No runtime
dependency or deployment change has been made.

Whole-scene model eligibility is now a reusable streaming visitor, independent
of chart allocation. `REMIX_DEBUG_BAKE_MODEL_PREFLIGHT` writes a fresh bounded
inventory and intentionally stops before GPU baking; see the
[study guide](model-chart-study.md#whole-scene-eligibility-inventory). Full bm_c1a1b
inventory completes with 2175 source draws, 9630 authored charts, 1279 accepted
draws, 531397 triangles, 612288 source-vertex references and 6433.60 square metres
of full admitted triangle area. It has 392 shared raw-geometry groups and 887
potential repeated draws. This is not an atlas fit or a lighting bake. Reuse must
account for transforms and eligible face subsets, and keep lighting per instance.

The extraction retains the model chart cap/cache policy. Five cached neutral
Construct views remain pixel-identical; nine of ten total neutral/shaded captures
match the older baseline exactly. One shaded view differs by at most one 8-bit
code value in 6347 pixels; two current-runtime repeats match each other exactly.
Cause is not established, so do not claim whole-frame bit identity. Both repeats
complete without validation errors and with zero grid/surface bake batches.
Next is actual whole-map atlas planning and bounded unwrap integration, followed
by lighting, controllable-group, binding and complete-cost checks before promotion.

Independent 8192-sample, three-bounce base-unstyled hemisphere references at 14
charted housing triangle centres find candidate-policy-3 atlas/reference luminance
ratios of 0.71-1.37, versus probe/reference ratios up to 14.42. This helped isolate
the missing coverage. Policy 5 still needs fixed-coordinate world-atlas/reference
comparisons, other maps, controllable-light and live-binding checks, memory and
complete-frame cost gates before promotion. Dynamic receivers remain on probes.

A smooth-edge connectivity reorder was tested and removed: housing chart count
increased from 27 to 29 without fixing the patches. Passing geometry/binding tests
was insufficient. Bounded receiver-ray diagnostics retain up to 256 triangles and
chart ranges per hit draw, with explicit truncation flags, for future diagnosis.

Keep caller-authored charts, retained-draw bindings, the shared atlas budget, PBR
and controllable-light behavior. Do not substitute runtime per-light direct work
for a fully baked static-model path. The VRF viewer's
[lighting-source selection](https://github.com/ValveResourceFormat/ValveResourceFormat/blob/a048d2cba7ccb94d584b83caf6f55ca66a177854/Renderer/Renderer/World/WorldLightingInfo.cs#L56-L72)
supports lightmaps and baked vertex lighting before probe fallback; this is viewer
implementation evidence, not a claim about every Valve engine configuration.

Dynamic-prop probe quality and side-aware volume interpolation remain required.
Static-model surface bakes do not qualify or replace those goals. Retain fixed
record/storage budgets, matching unstyled/component semantics, whole-field and
repeated-stream comparisons, work/fence ownership and whole-frame cost gates.

Use `scripts/common/compare-probe-receivers.py` for paired
`REMIX_DEBUG_BAKE_RECEIVERS` reports. It requires identical scenes, positions,
normals and surface references, compares common covered receivers, reports coverage
loss separately, and lists bounded worst regressions as well as improvements.
These triangle-weighted comparisons are a rejection gate, not ground-truth volume
lighting or full-map visual acceptance. Keep the repeated fixed-site stream gate.

Next gates: geometry-aware receiver-adjacent support and targeted convergence,
the existing barrel/crate references, multiple maps, complete bake-time cost and
unchanged physical record/storage budgets. Retain seam continuity, unknown
fallback, arbitrary controllable groups and whole-frame cost gates. Require
repeated fixed-site streams before promoting a sampling change. The ceiling
housing leak remains unresolved.

## Current automatic-cubemap delivery gates

The receiver-supported local replacement path remains opt-in. Completed functional
checks include cached capture/evidence restoration, corrupted-companion fallback,
priority/feather edits, explicit empty sets, sidecar removal and capture-origin edits.
A cached Construct origin edit moves one authored capture 16 Source units: revision 2
retraces exactly 98,304 rays (one 128px cubemap), reuses the other authored capture and
immutable transport, and replans local selection. Diffuse bake identity/content hashes
and the untouched authored record match the control; the original user sidecar remains
byte-identical. The isolated relay completes without validation errors or synchronization
hazards. This proves bounded recapture for this edit, not every origin or visual result.

Local-capture preparation is now independent of diagnostic report paths. A normal-quality
Construct legacy-cache upgrade produces the same companion bytes as report-backed preparation,
preserves the base cache, and performs no diffuse, direct-visibility or probe-depth bake work.
Its warm reload restores four local layers without capture or geometry rays. Both isolated
relay runs pass validation; CPU tests cover disabled requests, cancellation, report-only fresh
output protection and retained evidence despite existing reports. This completes the preparation
gate, not the remaining reflection-quality or default-publication gates.

A normal-quality, single-runtime Construct -> Flatgrass -> Construct relay sequence now
passes completed-scene transitions. Flatgrass publishes its own bake hash and two automatic
probes with no Construct-authored IDs. Returning restores all 64 Construct probe records and
all recorded lighting content hashes; the four paired captures are pixel-identical. The
Construct base cache and authored sidecar remain unchanged, preparation completes, and Vulkan
validation reports no errors or synchronization hazards. The relay now supports bounded map
sequences for repeatable lifecycle checks. This does not qualify live engine changelevel,
transitions during unfinished capture preparation, or the remaining visual-quality gates.

Remaining gates before default publication:
- Scene changes while local capture preparation is still in flight; completed relay transitions now pass.
- Underwater medium/path-length and projection mismatch against the path tracer.
  A fixed-box far/near-origin control implicates placement; residual colour parity remains open.
  Reassessed receiver-near refinements now integrate experimentally; cold/warm checks pass.
  Normal-quality Construct still selects a different underwater origin and retains an angular region.
- Moving-receiver/doorway quality, wall leakage and broader-map performance acceptance.

The offline proxy comparison can now propose localized influence bounds from sampled
receiver evidence (`--fit-error-metres`). A bounded seed-growth fallback avoids dead
ends in greedy inward cuts; neither method certifies free space. Training and held-out
receivers remain separate. Normal-quality Construct pool candidates produce proposals,
but both fail held-out checks at 0.25 m and 0.5 m spatial residual limits. Do not publish
these boxes or treat a lower median residual as visual acceptance. Improve capture
placement and geometry/medium evidence, then repeat moving-receiver rendering checks.
The successful hand-authored localized overlay remains a representation control,
not evidence that automatic bounds are ready. This diagnostic changes no defaults.

Earlier continuation paragraphs below retain historical limitations and measurements;
this checklist supersedes their now-completed cache and live-sidecar functional gates.

Current testing installation: `forwardplus-bake-batch-20261024`, native binary
`0f4a1ba` with adapter `09c6477`. It includes measured Refract imports, optical
tint motion/stationary bake policy, canonical Source asset paths and batched
bake pipeline compilation. Existing rendering defaults and user-authored
assets are preserved; experimental probe batching remains opt-in. Earlier
checkpoint/deployment descriptions below are historical. Focused fixed-view
Trainstation qualification passes; moving gameplay, wider visual parity and
the remaining roadmap are still open.

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

The intended result is a cheaper, Source-2-style rendering backend for GMod: forward PBR shading with
MSAA, hardware map-load surface lightmaps and probes, attributed baked direct lighting, soft static
shadows combined with dynamic-object shadows, automatic tolerant parallax cubemaps, rough SSR, and
optional hardware reflections/GI. Replacement materials must be shared with path tracing. Water must
remain compatible with the same authored optical materials.

This roadmap tracks delivery requirements. Detailed measurements and run evidence belong in local
reports. An implementation or synthetic test alone does not close an integration or visual-quality gate.

## Continuation status (2026-10-23)

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
This continuation is deployed in the verified testing refresh described below; broader gameplay
and roadmap quality gates remain open.

The separate GMod testing install now uses native `d9d54c2` and adapter `a499842`. The latest
refresh enables early-depth shading for eligible viewmodels and includes ordinary-water shader
source/binary parity plus the newer profiling/diagnostic groundwork. Only the two DLLs and runtime
manifest changed; the other 152 owned files and all ten protected settings/assets/launcher files
verify unchanged. The earlier world early-fragment rejection and reflection influence/feather updates
remain. Sparse sun depth and selected-probe batching stay opt-in; deployment does not enable them.
The root install record agrees with the runtime checkpoint, and deployment settings pin an immutable
verified package. Rollback copies remain available. All 155 owned files pass verification.

Normal-quality Construct before/staged/installed captures retain identical pixels/content hashes
with synchronization validation. The staged DLL passes late-emissive/TAA preparation and visibility
checks. Six current GPU suites cover MSAA, viewmodel overlay/shadows, baked probes, SSR, authored
edits and sun-depth lifecycle; F12 and sidecar CPU checks pass. These are functional checks, not
armed/moving gameplay or FPS acceptance. Automatic fallback outside authored volumes is now enabled
by default and pinned by the testing launcher; authored priority remains local. User backend settings
are preserved. Remaining transition work must close uncovered doorway strips using visibility-aware
influence overlap, without expanding projection bounds through walls or changing saved authored boxes.
Actual frozen receiver coverage must drive that work: displaced surfaces can sit below BSP base-plane
occupancy and be classified solid even above their rendered surface. Candidate ranking alone cannot
repair receivers outside every generated proxy. Add geometry-validated candidate/influence support
for these surfaces, preserve doorway wall rejection, and measure cost before changing the default.
Check both horizontal handoffs and vertical terrain/floor coverage rather than eye-height points alone.
The bounded geometry audit now retains finite first-hit envelopes per proxy face, using existing
readbacks rather than additional rays. These support geometry-aware fitting experiments but must not
be used directly as enlarged boxes: each proxy-face group can mix different real surfaces, so
localized receiver visibility and wall rejection remain required before automatic growth is accepted.
The receiver segment prototype now constructs normal-offset capture queries and conservatively
interprets existing hardware geometry readbacks, including unknown short misses and endpoint hits.
CPU and GPU wall/open-segment coverage is in place. Bounded nearest-capture planning now skips
covered samples, spreads budgeted receiver selection and preserves deterministic capture ties.
Optional planning reports expose automatic-set proposals without executing rays. A separate fresh-path
hardware audit now schedules bounded geometry batches, retains fence ownership and publishes segment
results without changing fitting or cache inputs. The audit now retries shortlist ranks 4..7 only
after four blocked results, sharing the original ray cap and retaining completed evidence. Unknown
or visible groups never trigger retries. Receivers still unsupported after bounded search now have
bounded normal-offset site proposals and spherical hardware admission are now available in the
optional diagnostic, prioritizing partition disagreements without treating occupancy as truth.
Admitted sites now receive bounded receiver connections and 10 cm axial clearance checks; blocked
or unknown results cannot authorize support. First-block positions distinguish overlap/buried
receivers from simple missing influence. Individual connection failures are not sufficient to remove
charts or declare camera invisibility. Next validate projection fits, capture-slot allocation and
visible receiver coverage before publishing candidates. Authored-composition pairing remains open.
Ordinary lighting cache identity is unchanged by this refresh;
optional sun-depth policy/layout changes can recapture depth while retaining cached irradiance.
Older cache files are not deleted. Broader map, quality and performance gates remain open.
The native continuation adds chart-edge denoising, stable progressive history, surface-hemisphere
transport, reflection-free lighting diagnostics and cosine-anchored surface/unit-light irradiance.
The deployed DLL passes a small Construct replay bake with synchronization validation. Chart-local
interior donor repair removes the reproduced ceiling-fixture halo in fresh neutral, raw and shaded
captures. Broader map, connector/secret-room and interactive quality acceptance remain open.

Live installed-runtime profiling now records Windows foreground ownership as well as Source's
focus flag. The latter can remain true while Windows reports the game unfocused; reject those
intervals. A process-scoped helper restores/focuses the owned benchmark once after readiness,
without repeatedly stealing focus. Frozen armed Construct hall and colour-room samples have
accepted foreground intervals and stable bake identity; this does not close moving gameplay gates.
The colour-room camera is trace-validated in non-solid space, looking at its `func_brush` shell.
That shell takes the probe-lit receiver path and dominates this view, unlike the largely lightmapped
hall. Separate bounded receiver queries also find substantial shader invocations on mapped geometry
with zero passing depth samples and no omitted query ranges. Ordinary prepassed world shading now
uses explicit early-fragment-test variants with read-only depth; the matching prepass owns depth and
coverage. Prepass-off, terrain, water, viewmodels and shadow shader policies remain separate. Six GPU
suites pass synchronization validation, including all alpha comparisons, draw orders and MSAA modes.
Opposite-order Construct live checks retain stable bakes and improve the expensive colour-room view;
full-resolution Construct and Crossfire replay comparisons also improve. These frozen-view results
do not close moving gameplay or wider map acceptance. Investigate whether
stationary controllable brush shells can enter a stable bake without treating movable/toggleable
geometry as immutable or adding Source-specific rules to OpenRemix. Clustering and local shadow
rendering are not the dominant cost in these captured Construct views; other maps remain unmeasured.
A lazy coarse-stencil variant preserves CPU/GPU reconstruction but remains rejected: it improves one
probe-heavy live view while regressing the ordinary hall replay. Its shader/generated-code changes
were removed; an explicit unsupported-fine GPU fallback regression remains. Keep the existing stencil
until a replacement improves representative workloads without trading away other views. The Construct
room shell is separate brush submodels with editable color/materials and disabled baked shadow casting;
a future stable receiver relay must preserve those policies independently of movement classification.
A four-case live colour-room diagnostic now isolates selected highlight and mixed-shadow evaluation
from the aggregate reconstruction. Both contribute substantial receiver shading cost while bake identity
stays stable; disabling them changes lighting and is not an accepted optimization. Keep their defaults.
Next evaluate bounded direct-address probe visibility with exact stored values, unknown-coverage and
attributed-field ownership preserved; compare it with the current ranked sparse rows on other maps.
Do not attribute all local-light cost to storage until a controlled candidate establishes the benefit.
The bounded byte-rank directory prototype preserves exact values but remains rejected: the white-room
repeats show no benefit despite a small hall gain. Its production and GPU-format changes were removed;
a wider-light-count GPU fixture remains. Compiler executable capture reports high register use in the
world/viewmodel fragment variants. Its local-memory byte values have implausible high bits, so they
are not accepted as spill-size evidence. Next investigate invocation-private stencil layout and lifetimes,
using normal-runtime timings and reconstruction checks. Human-readable prepassed/viewmodel pipeline
labels are now registered; compiler captures remain separate from performance acceptance.
The stencil-layout follow-up is now closed without a production change: four-corner vectors retain
high reported registers, fixed-component access regresses the hall, and offline Forward+ SPIR-V
optimization shows no normal-runtime benefit. All three variants and generated/policy changes were
removed. Keep the original readable stencil and compiler policy.
A pinned viewer re-review confirms per-light direct shading still exists; baked channels are sampled
once per receiver and reused by its culled barn-light evaluation. This is viewer evidence, not a claim
that Valve's implementation is identical. Next prototype batched selected-light probe visibility in
contiguous vector fields, preserving exact masks, custom stencil weights, unknown coverage and
attributed moving corrections. Key any derived field by active volume and selected captured IDs;
account for preparation/storage cost and frame ownership. Retain sparse fallback for unsupported or
large selections, and do not change highlight budgets to manufacture an equivalent-rendering win.
The GPU preparation primitive and first receiver integration are implemented in `forward/probe_batch`
and `shaders/probe-batch.glsl`. They remain opt-in via presence of `REMIX_DEBUG_PROBE_BATCH`; remove
that variable to disable the prototype (setting it to zero still enables it). The gather preserves
exact masks, negative unknown coverage, immutable bake ownership and ordered captured-light IDs.
Frame owners retain fields through their fences; reuse requires device-confirmed completion.
Preparation runs on the pipeline worker. Resident accounting includes parameters, alignment and old
submitted fields. Fields exceeding the bounded selection/payload limits retain sparse lookup.
Receiver shading reconstructs one four-light group at a time with the existing stencil. Attributed
moving-shadow corrections retain their directional lookup; live GI and water retain their own paths.
GPU checks cover both sparse formats, directional wrappers, padded table offsets, duplicate/missing
IDs, the full selection, budget rejection, cancellation and replacement lifetimes. Initial integration
checks cover world/MSAA, baked viewmodels and viewmodel SSR. A focused, armed Construct colour-room
opposite-order comparison improves both repeats at stable bake identity and unchanged light budgets;
fixed room regions retain matching lighting. The ordinary hall replay has no clear benefit and small
order-dependent differences. Keep the prototype off by default: validate moving selection changes and wider maps,
quantify preparation/resident cost under churn, and resolve the hall
tradeoff before default/deployment acceptance. Runtime replay validation reports the same unknown
newer device-feature structure in both on/off runs with the installed older validation layer; do not
call those runs wholly validation-clean. No new gather/descriptor/synchronization message was observed.
The testing install was restored byte-for-byte after owned diagnostic runs.
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
follow-up and use feature-off controls before changing production resource or shadow lifetime rules. This is an in-progress
optimization, not broad FPS/quality acceptance or a persistent bake-format change.
The branch also regenerates the stale ordinary-water fragment from current maintained source. Its
source/binary and SPIR-V checks plus four water GPU suites pass. This correction is included in the current testing refresh. Shared shader changes must account for ordinary water,
hardware water, live GI, Forward+ and their GPU query fixtures.

Runtime performance investigation must distinguish completed-bake gameplay from bake transport and
startup. The renderer already provides an opaque depth prepass and tiled, depth-sliced light lists.
The normal-quality Construct hall continuation confirms the user's two authored cubemap volumes
are active alongside complete directional coverage, no unrestricted analytic lights and no static
shadow updates. Profile normal-runtime CPU/GPU costs with validation disabled after correctness
checks; synchronization-validation overhead and changed submission pacing are not gameplay cost.
An early raster backface-cull variant for the depth prepass preserved coverage but failed opposite-order
timing comparisons with and without validation, and was removed. Continue with draw/vertex/setup
work and depth ordering while preserving colour/decal order and offscreen shadow casters.
Local pipeline/material/buffer binding reuse likewise preserved the sampled image but failed to
show a repeatable normal-runtime end-to-end benefit, and was removed. Keep rejected experiments
out of the shipping draw path; assess depth-only ordering separately from colour/decal order.
Stable nearest-bounds depth ordering also remains rejected: it preserved sampled coverage but
increased measured fragment work, and its full-resolution timing benefit varied with trial order.
The existing prepass-off option needs other maps and light-heavy/moving views before any default
change. Preserve the full-resolution normal-runtime baseline; small fixed-view differences are
not evidence of gameplay FPS parity.
Next isolate the dominant opaque receiver work with bounded fragment/receiver diagnostics, preserving
ordinary draw order and checking omitted ranges. Investigate the observed increase in local-shadow
GPU time when the independent baked-highlight budget is reduced: compare selected emitter identities,
caster submissions and timestamp boundaries before attributing the difference to extra shadow work.
The Crossfire check now confirms unchanged shadow identities and submissions across highlight budgets;
the timestamp shift alone is not evidence of additional caster work. Local point shadows now clip
casters against individual cube faces, retaining seam-crossing and offscreen actors. Frame reports
expose per-slot identities and recorded frozen/moving draws for continued profiling. Next isolate
covered versus uncovered probe receiver cost and the remaining surface-lightmap shading cost.
The current installed-runtime Construct hall replay, without synthetic probe receiver spheres,
measures about 0.66 ms in the depth prepass, 1.13 ms in opaque shading excluding depth, and
0.63 ms in reflection composition. Bounded receiver timing attributes only about 0.095 ms to
ordinary probe-lit geometry in that view. Do not carry the older sphere-heavy probe diagnosis
over to ordinary BSP geometry. Check constant-alpha depth material work next, and retain an
optimization only after coverage tests and matched runtime measurements. These fixed-view GPU
times do not establish live gameplay FPS or armed/moving-view performance. Constant-alpha depth
comparisons now skip albedo sampling while preserving camera-backface rejection and cutouts.
All eight alpha comparisons pass world/viewmodel, draw-order and 1/2/4-sample coverage checks.
Two opposite-order Construct hall comparisons reduce total GPU time by about 3â€“4%; keep this
scope separate from general performance acceptance. A further fixed-view Crossfire pair reduces
total GPU time by about 1.7% at matching clocks, with at most one channel value of image difference.
Viewmodel overlay/shadow isolation, baked-probe lighting and SSR compatibility tests pass with
synchronization validation. The installed-DLL smoke also passes; settings, mounts, adapter and authored
Construct volumes are preserved. Armed/moving gameplay performance and broader-map acceptance remain open.
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
| `bm_c1a1b` | Added complex-map case: bake time and memory, traversal stutter, light/probe coverage and reflection transitions. Black Mesa's packaged BSP contains authored `env_cubemap` entities with `parallax`, `boxmin` and `boxmax` fields; use these as a reference for automatic fitting and a future authored-volume importer. Runtime interpretation still needs verification. |
| Large combined Half-Life addon maps | Added long connected-layout stress case: room/portal transitions, probe and cubemap boundaries, cumulative bake work, memory and traversal stutter. Inventory installed BSP names first; include the `hls00amrl` authored-reference comparison where available, then select additional combined maps. |

For `bm_c1a1b`, the installed Black Mesa `bms_maps_dir.vpk` indexes the BSP in
`bms_maps_002.vpk`; verify required mounted assets before testing. Future authored
cubemap import belongs in the Source adapter: identify the map's volume representation, preserve
capture origins independently from projection bounds, and translate supported transforms and
priorities into the existing engine-agnostic `SetReflectionVolumesEXT` API. Validate coordinate
conventions and overlapping-volume behavior against the authored reference before enabling it.
Runtime import remains planned work; existing BSP cubemap capture seeds alone do not establish
authored parallax bounds support. The read-only `probe_editor.bsp_reflections` tool now extracts a
map-fingerprinted projection-reference corpus from loose BSPs, retaining raw capture/box/angle fields
and optional Hammer side assignments. Its bounded entity parser rejects malformed or ambiguous
qualifying records and selects only explicitly enabled parallax entities. Extraction from the installed
Black Mesa BSP matches the earlier manual reference, including its side assignments. Reference JSON
is deliberately distinct from authoring JSON and cannot be published as a runtime sidecar. Inspect the interaction with the entities' optional `sides` face assignments
before treating their boxes as freely blended influence volumes. Bounds values appear relative to
the capture origin, but confirm this convention against Black Mesa rendering before conversion.
The installed Black Mesa `bin/base.fgd` defines `boxmin`/`boxmax` as **projection** bounds and
`sides` as optional direct brush-face attachments. These are not evidence of receiver influence
bounds or overlap priorities. Version-one native authoring uses one box for both projection and
influence. The optional version-two sampling request now accepts independent influence bounds and
feather distance without changing the original records or sidecars. A faithful automatic importer
still needs a justified selection/influence policy rather than silently converting projection boxes
into selection boxes. Preserve source face assignments in reference
data; Hammer face IDs must not be treated as compiled BSP face indices. Only explicitly enabled
`parallax=1` entities qualify; default boxes on ordinary cubemaps are not authored room bounds.
The reference CLI also supports `--compiled-receivers`, preserving actual compiled face-to-material
assignments through validated face, texinfo, texdata and string-table indices. These include brush
models and displacement base faces; they do not assert visible receiver coverage. Packed material
patches can carry explicit capture-texture assignments, providing a separate basis for authored
static receiver selection. BSP runtime import and dynamic-object influence/blending remain
unfinished; do not infer those semantics from a material filename or projection box alone.
`--capture-assignments` additionally inventories explicit packed-material capture links using
bounded compiler-style patch/include/replace scalar blocks. It preserves missing, unsupported,
nonlocal and ambiguous statuses instead of guessing; matched origins identify projection-reference
entities, not dynamic influence regions. General VMT inheritance, BSP runtime import and
reference-renderer acceptance remain separate work.
Runtime receiver bindings must carry stable logical capture IDs rather than transient image-layer
indices: authored edits keep the previous capture set active while replacements prepare, and
composition can reorder records. Resolve bindings against the capture set actually sampled by the
frame. Reflection guides must preserve receiver selection from the same covered MSAA sample as
depth, normal and F0. Keep Source material/face interpretation in the adapter, dynamic influence
selection separate, and reflection-only edits outside diffuse bake invalidation. Required acceptance
includes reordered captures, pending edits, map changes, missing bindings and MSAA silhouettes.
Native authored publication now retains CPU-side stable IDs in actual priority-sorted/composed
image-layer order. Automatic fallback layers carry zero. Probe diagnostics expose each layer
identity and the published scene/revision separately from a pending request. This metadata follows
the active image while edits prepare; GPU layouts and diffuse bake inputs are unchanged. The
optional opaque-material `MaterialReflectionBindingEXT` now binds a source receiver to one stable
scene/volume ID. Source assignments survive USD/PBR shading replacements; instance material
overrides supply their own assignment. Forward+ resolves against the active publication, uses
ordinary selection for missing/wrong-scene IDs, and transports world selection through normalized
RGBA8 guide alpha from the same covered MSAA sample. Viewmodels use the same bound projection
and roughness mips directly. Explicit bindings bypass influence blending; SSR retains precedence.
Original BSP projection-bound import, dynamic selection, silhouette-specific selector acceptance
and map-wide performance qualification remain open. This is not deployed. Binding-only material
redefinitions now update a copy-on-write receiver table, separately snapshotted for each frame.
Draws retain their original material key through shading replacements. Shader materials, cached
retained geometry and frozen bake ownership stay unchanged when only IDs change; old frames keep
their own table revision.
The Source adapter retains explicit capture origins from resolved VMT `$envmap` values separately
from base texture upload hashes. Its sidecar watcher exposes only API-accepted documents (not
asynchronous image readiness), preserving prior metadata after invalid/refused edits. Exact unique
origin matching rejects coincident captures and integer-to-float aliases. Opt-in
`OPENREMIX_PROXY_CUBEMAP_RECEIVER_BINDINGS=1` now carries IDs through both live and relay material
creation. VMT receiver identities distinguish shared-texture variants; selected volume IDs are
metadata, not cache keys. Existing masks, PBR conversion, replacement keys and pins remain in
those paths. Owned optical, blended and modulated imports are excluded. Accepted-document revision
tracking avoids per-frame receiver scans. Same-map retained refreshes preserve authored requests;
actual map teardown clears them. Live GMod receiver resolution, identity edits and weapon changes
now pass functional qualification without diffuse invalidation; original Construct volumes still
use ordinary selection when capture origins do not match compiled assignments. Visual quality,
device-loss recovery, late-asset refreshes and paired-package checks remain gates before default.
Live Construct MSAA target changes now preserve the bake and authored publication with receiver
resolution intact; this functional check does not establish reflection quality or device-loss recovery.
GPU target-recreation coverage now verifies receiver pixels and diffuse ownership under synchronization
validation; CPU coverage includes shading refresh, deletion, hash reuse and scene reset isolation.
Compatible target recreation now shares authored capture ownership as well as diffuse baking.
Completed capture images and pending edits survive MSAA changes; the active renderer alone advances
frame-slot retirement, and incompatible device/queue/pipeline ownership is rejected before preparation.
GPU synchronization coverage verifies image identity and pending-edit cancellation across replacement. Runtime
capability negotiation now requires `GetReflectionBindingVersionEXT()` version 1 or newer; missing
or older contracts preserve ordinary selection rather than silently emitting ignored nodes. This
immutable query is available before initialization and does not establish device/capture readiness.
The original Black Mesa projection conventions are still unverified; manual sidecar matching does
not constitute a raw projection-bound importer.

Bake input readiness now follows retained brush/overlay/terrain/prop material dependencies rather
than the entire BSP material inventory. Required unresolved inputs still block publication. The
Trainstation visual/performance acceptance remains open: its now-supported `models/props_combine/com_shield001a` uses
Source `Refract` with no base texture, normal/tint maps and animation proxies. Native baking now
supports explicitly measured finite dielectric sheets: two-interface reflection/transmission,
textured absorption and containing-volume classification. CPU ownership/validation and hardware
transport tests cover both sides, distance independence and sheets inside a volume. Native live
sheet refraction now shares the late MSAA pass, with finite thickness, textured absorption, an exit
offset, visible-sky transmission and shared SSR/cubemap/hardware reflections. It assumes air outside
the primary sheet; camera-media/nested-layer integration and full path-tracer visual parity
remain pending. The newer paired candidate includes adapter material/proxy integration and
completes Trainstation bake/cache and moving-camera checks; it is now deployed for testing. Source's artistic refraction amount is not a physical IOR. Define a stable optical tint policy
explicitly: the tint sampler can share animated
bump UVs. Do not freeze an arbitrary animation frame, skip the dependency or mark a placeholder ready.
Check complete first-load sky capture and radiance separately once this gate opens.

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
been superseded by the reviewed testing checkpoint above. Fixed-view improvements and
component checks do not close live-game performance, broad probe quality, moving-viewmodel or
other-map acceptance. Continue those gates rather than treating testing deployment as release completion.

### Consolidated requested work and remaining acceptance

| User requirement / report | State and remaining work |
| --- | --- |
| Source-2-style cheaper backend; clean, modular readable code | Core Forward+ backend implemented. Review the new adaptive modules and lifetime/cancellation boundaries before release; no promise of live FPS parity with path tracing. |
| Fully baked direct + indirect on static surfaces and probes | Canonical baked runtime implemented, with attributed corrections, selected highlights and conservative unknown fallback. Finish live map-scale correctness/performance acceptance and retire only proven-redundant paths. |
| Scalable single-channel visibility/index layers, sharing nonoverlapping lights | Indexed R8/shared-page/compressed encodings and attributed directional fields implemented. Validate arbitrary overlap, budgets, cancellation, cache migration and unknown coverage; do not claim a confirmed CS2 four-light or 1024-unit rule. |
| Toggleable/named/styled lights baked and independently controlled | Grouped direct/indirect data implemented with scalar runtime control. Broaden live toggle/removal/replacement and mixed-shadow checks; native contracts must remain engine agnostic. |
| Surface atlases using a fixed memory/texture budget | Surface charts, world-space density and bounded page packing implemented. Quality varies. Fit density from the target budget rather than an arbitrary user luxel spacing. Inspect chart continuity, seams, page utilization, skybox weighting and backface waste; verify practical 2K/4K page caps within memory limits. Pages are intentional; one 4K atlas is a request to evaluate, not a requirement to discard paging. User later preferred 4K/compression over multi-GB allocations. Interior-donor repair prevents filtering leaks, but buried samples still consume planning, ray and atlas capacity; investigate earlier exclusion/reallocation without dropping visible surfaces. |
| Soft radial emitters, soft static shadows and antialiased direct bake | Finite emitter transport and sunlight refinement exist. Improve local/overflow and thin-shadow refinement, indirect noise and connected seams; compare path-traced emitter semantics. |
| Construct artifact reproduction | Target the two-ceiling-light room behind the usual spawn, its bent connector and the secret room, as identified by the user. Compare neutral baked irradiance with ordinary shading before attributing artifacts to atlas packing. The default chart rectangles occupy about 96.8% of the four-million-texel allocation in the current replay. A padded-chart denoising experiment did not affect production's zero-padding layout and was removed. Replay now supports static LDR sky faces and frozen live sky references for procedural skies; use the latter for Construct rather than accepting the old flat-background bake as an in-game match. |
| Denoiser chart-edge colour fringes | Reproduced blue/purple secret-room strips and connector bands in neutral irradiance with a frozen live sky. Temporary image alignment now continues each chart's edge data instead of injecting black HDR / minus-one signed SH padding; reproduced bands disappear without increasing atlas budget. CPU denoising, cache, atlas and GPU compatibility checks pass. Filtered cache identity changes; raw-bake identity remains unchanged. The testing install carries the fix, and ordinary-material replay with full SSR and auto exposure retains the repaired edges. Wider-map and temporal-exposure acceptance remain open. |
| Construct ceiling-fixture halo | Reproduced on the lightmapped ceiling above the two lamps behind spawn. Four ceiling samples inside the static housing see its emissive underside with 100% back-facing hits; their bright values spread outside the housing through lightmap filtering. Chart-local exterior donor repair removes the halo in fresh neutral, raw-without-OIDN and ordinary shaded captures without suppressing the lamp emission or increasing atlas/ray budgets. CPU and closed-fixture GPU regressions cover the repair. Mixed/styled publication, cache reuse and synchronization validation pass. Broader map and interactive acceptance remain open; the connector and secret-room artifact gates are separate. A fresh full-quality connector pair at position (2020,-744,-90), angles (-15,315,0), using neutral irradiance and ordinary shading, shows no obvious chart discontinuity or prior coloured edge fringe at that viewpoint. Both runs exit cleanly with 63 automatic probes and the same bake identity. This does not clear the farther bend or secret room; the exact remaining location needs confirmation. |
| Lighting diagnostic reflection isolation | Late water composition previously added opaque reflections to diagnostic views even when the water itself was occluded. Water composition and exposure now honor the same diagnostic exclusion as the final compositor. The SSR/MSAA regression compares occluded-water on/off for neutral and coverage views; ordinary water and exposure tests remain required. This corrects diagnostic evidence, not the remaining Construct fixture halo. |
| Construct fixture emission isolation | The contributor is `models/props_c17/industrialbellbottomon01`. Neither emission-sidedness restriction nor increased material-input resolution resolves its ceiling halo. Local packed-geometry tracing and actual bake readback identify hidden interior ceiling samples as the source of filter bleed. Interior donor repair runs before per-light/styled encoding and denoising, preserving common chart ownership for all fields. It requires at least 64 rays, at least 95% first-hit backfaces and no more than 1% misses. Charts with no exterior donor retain their original data. Validate broader maps and ambiguous geometry before treating all lightmap artifacts as resolved. |
| Surface irradiance at grazing angles | An isolated sphere near the receiver horizon reconstructed about 0.740 irradiance against the exact 0.00785 cosine integral. Surface transport now accumulates that integral along existing path/aggregate-direct samples and applies the minimum squared SH correction at the receiver normal. Serial/parallel sphere and emissive-patch regressions cover exact energy/physical bounds, front-facing response, directional sky response and unchanged volume SH. Volume probes retain full-sphere SH; surface local/sun unit-direct fields use the same receiver constraint without changing visibility metadata or storage layouts. The grazing correction alone retained the fixture halo; interior-donor repair addresses that separate cause. Full transport, mixed/styled publication and MSAA/compatibility checks pass with synchronization validation. Broader map acceptance remains open. |
| Surface back-hemisphere self-lighting | Surface receivers previously projected the whole sphere into low-order directional SH. A coplanar emissive-patch regression reproduced nonphysical front irradiance. Surface paths now sample the incident hemisphere with the same ray count; aggregate direct samples also reject rear directions. Zero-normal volume probes retain full-sphere lighting. Serial/parallel transport checks cover constant sky energy, directional response, retained front direct light and volume signal. Full transport validation passes. Shader bytes automatically change bake identity; no extra atlas memory is allocated. Broader map acceptance remains open. |
| Path-traced reference history and replay readiness | Unconditional wall-clock animation time invalidated scene history even without scrolling materials: actual replay reported one accumulated frame after 269 traced frames. The clock now advances only for active scrolling normal layers and freezes at time zero during progressive accumulation. A longer Construct reference retains one scene generation through thousands of accumulated frames. CPU layer-selection and focused live-water/progressive-history regression checks cover the policy. Backend/denoiser equality alone still does not prove all shader preparation finished; explicit replay preparation readiness remains to do. Validation-enabled GPU runs also report semaphore-state timeouts; resolve or characterize those before claiming clean validation. |
| Hardware-accelerated, fast map-load baking; OIDN like vrad3 | HW ray-query transport, budgeting, indexed visibility, bounded OIDN calibration/CPU queues implemented. Cold-map work/latency still need broader profiling. Do not mislabel GPU frame time or whole startup time as bake transport time. |
| Long overflow visibility stages; c1a0d noise; Lost Coast/Steamlab baking | Some bottlenecks and chart faults fixed. Keep d2_lostcoast, dm_steamlab and c1a0d on the regression matrix; these reports are not universally closed. Intersecting lights and range/unit conversions need transport parity review. |
| Static world excluded from mixed moving-shadow maps | Implemented caster membership and empty-map correction shortcuts. Verify terrain, static props, replacements, map transitions and viewmodels; keep offscreen dynamic casters. Leave engine frustum culling off as requested. |
| Mixed shadow fade, point-light filtering, wall leaks | Timed selection/highlight fades and filtering changes exist. User suggested about two seconds; verify live transitions, penumbrae and c1a0 close-wall pre-bake artifacts. Construct white-room toggleable lights are an important leak reproduction. Audit whether the ray-occluder fallback actually fixes specular leaks; retain it only with demonstrated utility. |
| Stable persistent caches; no rebake on camera/weapon changes | Cache validation and invalidation receipts exist. Investigate remaining spontaneous/weapon-change rebakes; distinguish real light/material edits from identity bugs. Warm replay parity is not proof that installed live caches always work. |
| Auto exposure and same tonemap path as path tracing | Shared postprocessing and exposure controls implemented. Refresh live adaptation, HDR/water and mode-switch acceptance; coverage diagnostics intentionally bypass automatic exposure. |
| Automatic tolerant parallax bounds, overlap/feathering, priorities | Default reflection budget is 64. Current normal-quality automatic comparisons publish 63 captures on Construct and 64 on Crossfire; authored overrides replace that set. Capacity improved hall coverage; automatic room fitting, bevel tolerance, doorway/wall separation, rotations and blending remain open. Capture point and influence/projection boxes are distinct. |
| Better dynamic probe quality in Construct's main room | Scalar probe sunlight smears the narrow roof-slit shadow on props. Sparse baked depth now captures and publishes into world shading under a diagnostic opt-in. The Construct narrow-opening case improves; caching, broad quality and performance acceptance remain before defaults change. See [baked sun visibility](#baked-sun-visibility-for-dynamic-receivers) for retained diagnosis, rejected approaches and acceptance gates. |
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
`bm_c1a1b` exposed complete attributed surface fields exceeding the encoded storage allowance after
substantial tracing/encoding work. Adaptive lossless 4x4/8x8 blocks now fit all of its declared fields
within the unchanged allowance; complete coverage and the sampled image are preserved. Extend map,
camera and steady-state performance checks before claiming general scalability or rollout acceptance.
Wider bounded field windows reduce scheduling handoffs on this map while preserving sampled output;
extend the same-quality checks to other maps. Repeated encoding and remaining transport/visibility
stages remain bake-time optimization targets. Keep the existing
atomic fallback and bounded storage when improving them. The direct-light cache identity includes
the new codec capability so previously declined bakes are retried once. Its Black Mesa material
inputs also require ABGR8888 decoding and texture references with an explicit `.vtf` suffix, now
handled by the adapter. Successful cache restoration alone does not close lighting-quality or
steady-state performance acceptance for this map.
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

Partially charted opaque meshes now specialize consecutive mapped/unmapped
triangle ranges while preserving draw order. At most sixteen ranges are emitted;
fragmented meshes and mixed-corner triangles retain the generic shader. Depth,
terrain, shadow and live-GI handling stays separate. Continue broader-map and
moving-gameplay frame-cost checks; this does not promote static model charts.
Keep static-model surface-lightmap qualification distinct from dynamic-prop
volume support, particularly for narrow gaps with no clear stencil contributor.


World, terrain and depth command recording now retain identical consecutive
pipeline, material and vertex/index bindings within each invocation. Draw order
and per-draw push constants are preserved. Continue cross-map and foreground
live-game qualification; this removes redundant renderer calls without replacing
the broader engine capture/submission investigation.

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

`remixapi_ReflectionVolumesSamplingInfoEXT` extends authoring through the existing export: pass
`&request.base`, set its version to `REMIXAPI_REFLECTION_VOLUMES_SAMPLING_VERSION_EXT`, and provide
one sampling record per original volume. Projection, rigid frame, capture point and priority retain
their original meanings. Sampling bounds are local to that frame; the capture need not lie inside
influence. A nonnegative scene-unit blend distance sets feather width, with zero deriving the existing
20%-of-half-extent rule. Version-one callers retain combined bounds and their legacy half-metre
feather. The scene owns validated copies; invalid edits preserve the last valid request. Influence
and feather edits reuse existing captured radiance and do not alter frozen diffuse lighting.
The adapter now accepts both legacy `BSPCUBE1` and extended `BSPCUBE2` sidecars. Extended records
carry sampling bounds/feather metadata alongside unchanged projection records, with fingerprint,
capacity, atomic publication and retry semantics preserved. Editor JSON can retain optional sampling
metadata and imports representable probe-report influence/feather fields; legacy-equivalent exports
stay version one. Mixed exports preserve legacy feathering for unchanged volumes. A Construct
relay replay verifies independent projection/influence metadata and explicit feather distance through
the actual sidecar-to-native path. Matching updated adapter/runtime builds are required for version
two. Independent viewport handles and Black Mesa entity/face-assignment import remain tool work.

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
Authored sets replace the automatic set by default and in the deployed checkpoint. An explicit
`rtx.native.forward.authoredReflectionFallback` option now composes existing automatic image layers
behind authored priorities. It retains all authored records and removes automatic records only when
their entire influence box is proven contained by one authored box; remaining automatic records keep
their original order within the shared 64-record budget. Diagnostics report retained, covered and
capacity-omitted counts. Empty authored sets still disable local probes; null resets restore automatic
placement. The mode is present in the testing install but remains off by default and unaccepted across maps.
An installed-sidecar containment check confirms the skylit hall camera is inside the first authored
volume, while the behind-spawn room, connector and unconfirmed secret approach cameras are outside
both. Only 8 of the current 63 automatic capture origins are inside these volumes; that is an origin
membership check, not a surface-area coverage estimate. Replacement can explain missing local
cubemaps elsewhere, but does not explain weak main-hall reflections. Compare completed authored
and automatic hall captures with SSR disabled before blaming its bounds or radiance.
That comparison now confirms a completed two-probe authored set in the hall, with all 196,608
capture rays returned; local reflections remain visible with SSR disabled. Existing ordinary
materials are insufficient to accept capture sharpness, prop response or moving transitions.
Replay diagnostics previously retained initial diffuse-publication state while reflection-only
work continued. The optional bake-statistics tail now exposes requested/pending/active/failed
reflection state; measurement and camera-path readiness wait for publication, and final measurements
refresh their probe report. Legacy 256-byte statistics clients retain their allocation boundary.
An earlier report showing pending work is not proof that its later image was incomplete; match
fresh final diagnostics before making that claim. Fallback composition now has CPU selection, exact
GPU copy/readback and production authored-edit tests at 1x/4x MSAA. Those tests verify outside-volume
reflections, zero new capture rays on mode changes and unchanged diffuse bake identity. Map-scale
coverage and memory/performance acceptance remain open. A full-quality behind-spawn Construct check
now publishes 2 authored plus 59 automatic probes, with 4 automatic influence boxes fully covered
and none capacity-omitted. A glossy test sphere outside both authored volumes regains local
reflections with SSR disabled. The longer-warmup repeat exits cleanly with a fresh report and matching
image; its radiance hashes differ despite matching identity, geometry and capture inputs. The initial
short-window run failed only its final report-freshness drain. This is narrow coverage acceptance,
not a performance result or whole-map quality acceptance. A separate controlled 33-frame crossing
through an artificial interior authored boundary now verifies fallback feathering: sphere-interior
maximum mean adjacent RGB change drops from 68.46 display levels to 3.47. Both full-quality runs
exit cleanly with synchronized validation; fallback retains 2 authored plus 61 automatic records.
The original user bounds remain untouched. This verifies the constructed transition, not room-fitting
accuracy or FPS. Exact GPU readback also covers 64 interleaved/reversed source layers and all mips.
The input/screenshot fixture now explicitly selects raster, checks the actual DLSS reconstruction
default, and waits for the single completed presentation and resized screenshot before examining
frame accounting. Exceptions unwind an explicit runtime shutdown; thread assertions propagate to
the same cleanup path. Asynchronous, synchronous and deliberate failure-after-handoff cases pass
with synchronization validation; their decoded 400Ã—300 screenshots match. These checks used the
normal Meson object linked to a separate local executable because the old executable remains locked.
Earlier terminated tests still expose one blocked thread each to Windows wait-chain inspection,
with no cycle or wait owner reported. Their process handles remain unsignalled despite exit code 1.
Cleanup and an ordinary full API-suite run remain unconfirmed; do not accept new timings yet.
Windows GPU counters now confirm roughly 248 MiB dedicated plus 210 MiB shared memory retained
across the three old terminated tests. Three one-second engine samples show zero observed activity;
this does not explain the stalled cleanup or establish clean profiling conditions. A convenient
Windows restart has been requested; independent renderer/quality work can continue.
The rendering menu now explains that active authored volumes replace the whole automatic set.
The explicit fallback mode preserves replacement/empty-set/reset semantics and uses the priority
feathering below. It copies published automatic radiance without retracing, keeps canonical authored
captures available for edit reuse, and retains both image sources through the upload fence. Its composed
GPU image duplicates selected radiance while automatic images remain available for reset; include this
memory cost when accepting the mode. Completed authored capture jobs now release their CPU ray
and owner vectors after readback, before filtering/image allocation. Work ownership now stores one
index per capture instead of per ray: at 64 distinct 128-face captures its payload drops from
24 MiB to 256 bytes. Ray planning reserves only the unique new capture count, and avoids zeroing
radiance arrays before reuse/alias copies. The ray payload is still 192 MiB at that size and is
released before filtering. Both automatic and authored planning now append rays directly into the
reserved work buffer, removing a separate six-face temporary/copy per capture (3 MiB at 128-face,
48 MiB at 512-face). Face/texel order and ray bits remain unchanged; CPU alias/cancellation and
production automatic/authored rendering checks pass. These are array payload estimates, not measured whole-process reductions
or FPS results. Automatic bake capture scratch is also released after readback, before reflection
filtering, denoising and atlas encoding. Its exact input hash is computed once on the preparation
worker and retained as a scalar, avoiding a large-buffer hash during render-thread publication.
Production 1x/4x tests verify the published hash against exact reconstructed ray bytes. This latest
automatic cleanup is included in the `ffc32fe` testing-install refresh. Cold/warm Construct
validation caught and fixed a missing cached-load provenance step in that cleanup: both preparation
paths now retain the exact input hash and release scratch. A fixed warm load reuses the original cache
with identical content hashes and pixels; the 1x/4x production fixture now explicitly tests disk-cache
reload provenance. Warm preparation now computes that exact hash one unique capture at a time
using bounded incremental hashing, instead of regenerating the whole scene ray buffer. At 128-face
resolution its temporary ray payload is 3 MiB regardless of capture count (48 MiB at 512-face).
Streaming/one-shot equivalence tests cover block/tail boundaries and capture alias order; a Construct
warm load retains every cold content hash and pixel. No cache format or lighting change is required.
The installed checkpoint predates these source-only changes and the caught regression. Scalar counters and
canonical radiance remain available for edits.
Production 1x/4x tests confirm zero published transport scratch, retained pending scratch and complete
GPU image/acceleration cleanup after override reset and frame retirement. Diffuse bake ownership
remains unchanged.
Priority feathering is now implemented within a published reflection set: core priority remains dominant,
edge bands blend toward lower priorities, and shared floor/wall bands cancel through a relative coverage
ratio. GPU uploads sort metadata and radiance layers together; canonical capture/cache identities remain
unchanged, while diagnostics and viewmodel masks use the uploaded order. Analytic two/three-tier/HDR
checks and GPU projection queries across 8â€“512 resolution pass, as do authored-edit reuse, viewmodel,
SSR, water and sky-fallback checks with synchronization validation. These changes now ship in the
testing install; broad gameplay and performance acceptance remain open.
An isolated runtime built from normal Meson inputs now reproduces the intended Construct boundary
blend on a glossy receiver, while the low-only and high-core control interiors remain pixel-identical.
Both authored captures are fully published in the compared images. A camera-path smoke across that
boundary also passes synchronization validation. A subsequent 33-frame controlled glossy traversal
confirms a less abrupt transition, with matching start/end control interiors and clean validation.
The replay capture interval is configurable with a 64-image limit, retaining its default cadence.
Some sharp reflection changes remain identically in both samplers and need separate capture/filter
review; the priority fix is not proof that all glossy aliasing is solved. Other maps, material/roughness
variants, real gameplay motion and performance acceptance remain open. It does not compose automatic captures
into an authored override or fix coverage outside that override.

Expanded tests exposed a three-face corner discontinuity in the previous manual filter. CPU mip
preparation and the shared shader now average the diagonal corner tap across the three meeting
faces. All eight corner approaches have an explicit regression test. Policy 28 introduced this fix;
policy 29 also prevents reuse of sets truncated by the old buffer-capacity policy; current policy 30
additionally repairs hidden interior surface donors before filtering.
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

### Cubemap reference review follow-up

A pinned ValveResourceFormat renderer review identifies two concrete quality studies: its environment
lookup bends rough reflections toward the surface normal and uses per-capture L1 luminance SH to
normalize/occlude cubemap lighting against receiver irradiance. OpenRemix currently samples the mirror
direction through its independently GGX-filtered mip chain and has no equivalent normalization field.
The viewer also uses a different roughness-to-mip remap; copying that scalar without matching its
asset prefilter convention is not justified. Our base mip copies raw capture radiance, so roughness-zero
sharpness should first be checked against capture resolution and parallax rather than blaming GGX.
Treat this as an unofficial viewer reference, not proof of Valve's proprietary implementation.
Next compare controlled roughness/grazing sweeps and bright/dark receiver transitions, including props,
viewmodels, water and sky. Preserve direct visibility and validate energy, cache/layout compatibility
and actual map quality before introducing normalization or changing defaults.

A controlled analytic GGX/Smith study now compares mirror-direction prefiltering with viewer-style
dominant-direction lookup across 30 roughness/view-angle cases. Increasing quadrature from 131,072
to 524,288 samples retains the same tradeoff: mean environment/centroid error improves, but seven
narrow-highlight cases regress at low/mid roughness. Normal-incidence controls remain identical.
This normalized F=1 model omits actual cube texels, parallax, Fresnel and mip interpolation, so it
does not establish a production improvement. Keep the existing default while preparing actual
renderer roughness/grazing sweeps; do not infer parity or a universal fix from the average result.

Offline capture acceptance now requires the independent preparation-statistics export: loading must
clear, requested/completed pipeline counts must agree, and compilation failures or preparation
restarting after warmup reject the run. Existing public API layouts remain unchanged. Earlier images
with compilation overlays are rejected; the prepared references described below supersede them.

### Reflex validation isolation

The path-traced timeline warning is now isolated to the NVIDIA Reflex pacing path with the installed
1.3.280 validation layer. Repeating the same preparation-gated Construct capture with only
`rtx.native.lowLatency=0` removes the diagnostics and reaches 35/35 pipelines without a loading
overlay. The Vulkan specification describes this driver-signalled timeline/host-wait pattern, and
current validation-layer source explicitly tracks `vkLatencySleepNV`; the installed-version source
lacks that hook. This strongly supports a layer bookkeeping incompatibility rather than evidence
that graphics/bake synchronization needs weakening. Preserve game pacing defaults. Offline reference
checks may explicitly disable Reflex and record that scope; newer-layer validation remains desirable.
The clean isolated capture is functional/quality evidence, not FPS acceptance, and does not diagnose
the older retained-process cleanup issue. No renderer, driver or installed-setting change was made.


### Current cubemap quality evidence and next work

The initial six-roughness Construct study used only 245 probes. Its large bright highlight was direct
sunlight, not a cubemap-energy failure: a direct-specular pass retained it, omitting sunlight removed
it, and restoring the production shader with 31919 usable probes also removed it. Sparse-grid sun
visibility robustness remains an independent concern; do not infer reflection normalization failure
from that configuration.

The repeated dense-grid study has eighteen preparation-gated, synchronization-validation-clean
Forward+ captures: current shading, dominant reflection direction, and numerical GGX split-sum energy
response across six roughnesses. The same two authored captures finish in every case; reported baked
content/probe metadata agree. Six prepared finite-sample path-traced references provide a quality
comparison, with explicit Reflex-off validation scope. No production shader/default changed.

Dominant direction improves the fully rough sphere's mean absolute display-space RGB error from
6.43 to 4.13 in its core and 9.05 to 4.37 at its rim, but worsens middle roughness: at 0.6, core error
increases from 16.99 to 18.17 and rim error from 13.11 to 15.52. Numerical split-sum energy is also
inconsistent. Retain the current default. This fixed camera, metallic sphere, 128-face resolution and
lower-sample capture/transport study establishes neither general quality parity nor FPS acceptance.

Transport isolation now explains most of the remaining dark reflection mismatch. Raising face
resolution from 128 to 512 sharpens detail without fixing the dark ceiling. Keeping 64 samples but
raising transport from one to three bounces reduces the mirror core display-space error from 25.61
to 3.23; the existing 256-sample, three-bounce default reaches 2.39. At roughness 0.4 the default
transport gives 2.27 versus 21.93 for the cheap one-bounce case. These runs complete preparation,
authored captures and synchronization validation. Runtime defaults already use this transport, so
no default change is warranted. Earlier one-bounce direction/energy comparisons are confounded by
unconverged captured radiance and cannot establish production improvement or regression.

The adapter supplies a documented quality-study preset using normal transport and probe budgets.
Use it instead of smoke-test settings for visual acceptance. Future direction/energy candidates
need normal-quality comparisons; capture-origin/parallax and receiver-local controls, bright/dark
transitions, props, water, viewmodels, other maps and motion remain open. Preserve finite-sample
reference limitations and avoid treating this fixed metallic-sphere study as renderer parity or FPS.


### Static HDR sky relay

The BSP replay now handles floating-point/integer HDR sky faces and single-texture RGBS encoding,
including Source's radiance scaling and linear colour tint. The native API accepts immutable half/float
RGBA uploads through the existing HDR image path without changing ABI layouts. HDR sky interpolation
and transport preserve values above one; ordinary and frozen LDR paths remain supported. Legacy
three-texture HDR, animated/procedural skies and server overrides remain outside static decoding;
procedural skies still need an explicit frozen live reference. Normal game capture is unchanged.

The cold path-traced NOT_READY failure was traced to late emissive discovery changing temporal
lighting layouts in place after activation. Emissive-source eligibility now joins the existing local
source/camera resource key and compiler-worker preparation. The active renderer retains combined
lighting while its replacement is prepared; emissive visibility changes keep the accepted layout.
The old render-time emissive layout flag was removed rather than retaining two competing policies.

The original failure is reproduced with one compiler worker. The fixed renderer remains active for
that stress run's 100000-frame budget, though its optional shader queue outlasts that budget; that run
is not an accepted capture. A complete four-worker repeat reaches 57/57 pipelines and passes
synchronization validation. A public-API regression adds an emitter after startup, verifies continued
presentation and TAA, then warms lazy light-sampler variants and checks repeated hide/show cycles
without more renderer preparation or temporal allocation changes. That regression and a pixel/hash-
identical Forward+ comparison pass validation; pipeline preparation CPU checks also pass. These are
stability/compatibility checks, not FPS acceptance. Older rejected controls hit validation-layer queue
state timeouts during cleanup; that separate issue is not claimed fixed.

## Baked sun visibility for dynamic receivers

Target is the diagonal roof-slit skylit hall, especially props. High-detail matte receiver
comparisons now separate low-poly reference self-shadow artifacts from remaining probe errors. The
shaded case is close, but a direct-sun-only reference preserves the opening rim mismatch even beyond
moving-CSM range, implicating static sunlight visibility reconstruction. A dense local field
improves shaded-centre leakage but worsens the rim region; do not raise global density/brightness
from that mixed result. Position-only scalar sun sampling leaves shaded-centre leakage unchanged and
worsens the rim comparison, so retain the shared normal-dependent stencil. An uncached prototype
capturing coarse sunlight before refinement and adding its visibility range to allocation changed
the grid but left the targeted sphere error unchanged; it is not promoted. A second uncached
prototype reusing surface sunlight before placement and scoring its reconstruction error also
changed allocation without changing the target sphere result; retain neither prototype as a default.
A published-grid audit now confirms the affected cell uses 4 m refinement for a 0.81 m diagnostic
sphere. Production CPU stencil reconstruction gives 7.18% sunlight on its camera-facing side from
one sunlit node about 4.5 m away among shaded neighbours; the top gets 4.80%. This establishes
spatial smearing of scalar sunlight visibility at this location.

A diagnostic fallback to full static+moving sun cascades removes the false-lit sphere centre and
restores the thin rim closely (sun-only core error zero, upper-rim RGB error about 2.4/2.3/2.2
display-byte levels versus roughly 38/37/34). This confirms a spatial visibility representation
target rather than a global brightness correction. Keep the cascade control for quality comparisons,
but do not promote camera-following static cascades as the fully baked solution: camera movement can
redraw frozen geometry. Next evaluate baked directional visibility with local boundary detail, fixed
memory and bake-time costs; global allocation changes alone have not improved this receiver.

A conservative Construct probe-domain projection spans roughly 382 by 511 m: one 4K D32 map costs 64
MiB at 9–12 cm per texel, while 8K costs 256 MiB at 5–6 cm. These are domain-box estimates, not
tight receiver bounds or measured image-quality guarantees.

The optional receiver diagnostic now measures all charted triangle vertices in an orthonormal sun
basis (world transforms and metre scale applied once), including exact-cache-hit layouts. Construct
has 37,813 charted triangles spanning about 302 by 271 m in that projection. This is a tighter
bounding rectangle, not occupied-tile coverage and not a guarantee of dynamic receiver or static
occluder coverage. Conservative triangle/tile overlap at 4 m tiles reports 2,902 occupied of 5,244
bounding tiles; a 1 m projected L-infinity margin yields 2,980/5,313, and 2 m yields 3,060/5,460.
The 1 m case implies about 73 MiB raw D32 at 5 cm texels before borders, indexing and allocation
padding; this is a storage estimate, not accepted shadow quality or complete dynamic-receiver
coverage. Diagnostics enforce tile and candidate-work budgets and stop rather than silently omitting
geometry.

A deterministic CPU atlas planner converts complete requested tile sets into bordered physical
pages and a bounded virtual directory, declining the whole plan when its storage/directory/page budget
fails. Production fitting treats 2048 as a maximum and chooses whole-tile page sides that minimize
resident depth bytes, preferring fewer pages on a tie. These are linear storage-buffer regions and
need no power-of-two padding. Coverage, 80 interior texels per 4 m tile, two-texel borders and the
128 MiB budget remain unchanged. Tiny scenes no longer reserve a full maximum-sized page.
Layout planning remains independent from GPU resource ownership and capture scheduling.

Fractional filter weights are computed in local tile coordinates before adding the physical atlas
offset, preventing allocation-dependent floating-point rounding. GPU repacking comparisons and a
controlled Construct fixed/compact-page replay produce identical visibility/output with validation.
Page compaction reduces resident storage and full-atlas staging/readback volume; it preserves the
number of captured rays and the already-packed on-disk tile rows. No FPS improvement is implied.

A focused hardware prototype now captures a bordered sparse orthographic depth atlas through the
existing material-aware geometry transport: 2,304 rays resolve a 15 cm roof opening and eight depth
comparisons agree with independent direct-sun queries below/above the roof, including a fully
cut-out foreground sheet, with clean Vulkan validation. The initial fixture used CPU lookup; the
shared GPU lookup described below supersedes that test path. The world-renderer prototype now
exercises the actual Construct opening; broad acceptance remains open.

Compact capture now has a separately prepared optional kernel and `Transport::recordDepth`: one
signed 4-byte first-hit distance per ray writes directly into caller-owned GPU storage at a supplied
word offset, with an explicit negative miss and fragment/compute/transfer visibility barriers. The
focused test compares compact/full geometry results, checks range guards and invalid requests,
rotates frame slots and restores normal sun/geometry output descriptors after depth dispatch. A
shared GLSL lookup now reads the captured GPU atlas through validated scene-unit projection metadata
and its sparse directory. Four depth comparisons are filtered as visibility; a negative miss is
exposed, while missing tiles, empty maps and out-of-range receivers report no coverage for fallback.
GPU tests exercise the narrow opening at two heights, rotated bases, a half-visible filter boundary
and uncovered cases. Atlas population and lookup stay on GPU in the test (with verification
readbacks only). World fragments now use the same lookup when a matching map is published; uncovered
positions retain the original probe/shadow fallback. Ordinary transport does not prepare the optional
kernel. Capture output is 64 times smaller per ray, not a measured whole-bake speedup.

The renderer prototype is enabled only by the process environment flag
`REMIX_DEBUG_BAKE_SUN_DEPTH`. It reuses the ordinary lighting cache and stores depth in a separate
versioned sidecar. Receiver projection is a reusable CPU planning
module; `SunDepth` owns the immutable layout and GPU buffers, while a dedicated controller stage
schedules bounded indexed ray batches. Each ray writes its final atlas location directly. All
submission fences must complete before the published volume can expose the buffers. Old frames
retain volume owners, and memory accounting deduplicates shared capture/publication resources.
Projection and depth limits use scene units consistently and include the complete static caster
span. Viewmodels retain their existing probe and isolated close-up shadow path.

Construct's sun-only opening comparison removes the false-lit receiver centre and restores the thin
rim close to the path-traced control. Normal shaded output also improves the rim and most colour
errors, while residual indirect probe tint remains. These are fixed-view diagnostic results, not
live gameplay FPS or universal map acceptance. Disabled-path pixels remain identical; world/MSAA and
viewmodel regressions pass with Vulkan synchronization validation.

The versioned CPU cache chunk now packs only captured tile rows, retaining exact float bits and
restoring unused atlas space to explicit misses. It reuses the checksummed container and compares
sampling metadata and the sparse directory against the current scene plan before accepting bounded
sample counts. Producer and consumer independently reject nonfinite/out-of-range depth and unknown
negative sentinels; cancellation and malformed/truncated payload tests pass. Live capture copies depth
to readback storage once, waits for its submission fence, then writes and atomically replaces the
sidecar on a worker. Restore stages a GPU upload before publication and retains staging until its
fence completes. Cache identity includes scene content, capture shader, sampling layout and policy.
A missing or rejected sidecar captures depth without discarding cached irradiance or styled lighting.
Construct's partial-cache and warm-cache images match each other and the original sun-depth control
exactly with synchronization validation. The warm load restores depth without geometry tracing;
the existing lighting cache remains unchanged. This proves this fixed-view persistence path, not
default readiness or a general load-time/FPS improvement.

Production-owner GPU tests now cover receivers crossing the roof opening without recapture,
independent replacement allocations, retained-buffer lookup after replacement release, cancellation,
and explicit rejection of over-budget coverage. Tiling exhaustion declines the optional field rather
than failing the ordinary lighting bake. These resource-level checks do not replace live map-change
and moving-shadow acceptance.

A first-hit field cannot represent coloured dielectric transmission or proportional source-over
coverage. Capture now marks those nearest nonempty interfaces as unresolved rather than freezing a
stochastic alpha decision or treating glass as opaque. Any unresolved filter tap requests the existing
probe/shadow fallback; empty coverage remains a miss and fully opaque coverage retains exact depth.
The version-2 sidecar preserves this explicit sentinel and rejects old payload semantics. GPU fixtures
cover glass, partial/empty/full source-over coverage, empty cutouts, opaque hits and lookup fallback.
This is material-aware coverage gating, not layered coloured transmission in the depth field. Retain
that limitation and recheck real-map glass/water alongside the live transition acceptance.

A controller-level render fixture now cancels both tracing and cache-save phases, replaces frozen
scene lighting while retaining the old volume, verifies warm depth restore without capture, compares
cold/warm unmapped receiver pixels, replaces MSAA attachments and unloads. Synchronization validation
passes. This covers synthetic controller publication/lifetime, not large real-map transitions or
interactive moving-shadow quality.

Finite-source quality now has an independent hardware angular-reference fixture: a narrow roof slit,
two receiver heights and point/small/broad sun discs. The reference resolves separation-dependent
penumbrae that the centre-direction depth plus bilinear comparison cannot reproduce. Both roof
fixtures explicitly clear the default Y sun direction so the intended vertical geometry is tested.
Next evaluate blocker-aware visibility filtering against this reference, including search/filter taps
across sparse tile boundaries and missing coverage. A fixed wider blur is not finite-source support;
keep this acceptance gate open and measure lookup cost before changing the default.

A test-only sparse PCSS candidate now searches blockers and filters visibility comparisons with a
separation-dependent sun-disc radius. Each tap resolves its virtual tile independently; unknown
transmission or missing search/filter coverage requests fallback. Point-source lookup remains exact.
The candidate reduces broad-source fixture error but retains substantial error: the centre-depth
grid has already lost subtexel slit coverage, which filtering cannot reconstruct. Do not integrate
this candidate into renderer shaders or treat the prototype tests as quality acceptance. Next compare
coverage-aware capture and finer sampling within the existing memory budget before selecting a filter.

Next cover live map transitions and moving actors,
and measure the steady-state and total bake costs. Confirm unsupported/over-budget fallback,
transmissive casters and finite sun shape before enabling a default. The existing static-cascade
fallback can still redraw for receivers that need it; do not claim that all static sun shadow work
has been removed.

Use an atlas or grouped pages rather than assuming one array layer per occupied tile fits device
limits; retain distant static occluders in depth coverage and keep moving shadows independent. Use
`replay.receiverSegments=128` for close-up references; the original diagnostic default stays
unchanged. Moving props, viewmodels, other materials/maps and comparison with the earlier uniform
checkpoint remain open.


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


### Receiver-supported cubemap fitting follow-up

The optional receiver diagnostic compares strict occupancy with the ordinary renderer's configured
bounded-detail tolerance at the same supported origins. Strict alternatives remain controls; up to
six distinct projection/influence alternatives share each origin's geometry rays. Refined proposals
must revalidate against their own occupancy policy, including the allowed ignored-volume fraction.
This does not relax published fitting or admit solid origins. Larger tolerant alternatives are not
automatically better: retain strict controls and compare actual geometry error and receiver coverage
before choosing either policy.

The optional receiver diagnostic now compares a bounded set of supported local origins
with the existing strict partition fitter. It records independent projection and influence
bounds and whether each influence includes its originating surface sample. Sampled
hardware support never overrides a solid partition origin. No new captures are published.
The optional pass now assesses these local fits against actual geometry using shared first-hit
rays per origin and the existing projection evaluator. Near geometry, far geometry, misses and
backfaces remain separate rather than becoming a single pass/fail score. Next compare marginal
receiver coverage and projection quality within the existing capture budget, then qualify visible room
transitions and wall leakage. A blocked receiver connection is not proof that the receiver
is camera-invisible; do not remove bake charts on that evidence alone.

The local-site geometry comparison exposed a backface classification discrepancy: spherical
admission used interpolated surface normals (excluding dielectrics), while geometry-only
queries used hardware triangle facing. A hardware regression reproduced opposite classifications
on a double-sided wall whose normals oppose its winding. Geometry-only queries now share
admission's normal-based solid-side rule; material culling still uses hardware facing.
Shader regeneration/validation and hardware transport checks cover this contract. Compare the
resulting local-fit statistics before selection or publication; sampled evidence remains insufficient
to discard receivers or loosen occupancy.

Local-fit coverage now has an opt-in bounded geometry audit rather than treating influence-box
containment as usable coverage. Receiver/origin segments are shared across alternate shapes and
only target uncovered automatic-baseline receivers. Reports retain blocked and unknown outcomes
and local attempt ownership. Marginal analysis must use the final authored-plus-automatic baseline,
visibility-supported sampled gains and losses from displaced captures within the 64-layer budget.
Visual qualification uses isolated authored fixtures, SSR-off isolation, SSR-on integration and an
existing authored-room control. Preserve original volume identities and geometry when checking that
free slots can be used without displacing the baseline. Authored-fixture priorities do not implement
or qualify a production automatic-selection policy.

Local-fit diagnostics now retain optional weighted face-coordinate percentiles and sample support
from the existing geometry rays. Compare the 10th-to-90th spread and median against the current
proxy face before considering a plane shift. Sparse near-hit clutter and broad terrain distributions
must remain distinguishable. A percentile alone cannot classify dielectric interfaces or certify a wall.

An optional inward-only refinement proposal now uses named sample-support and relative-uncertainty
thresholds. It cannot enlarge the validated box or move the origin onto a face, and does not change
receiver influence. The local report reassesses proposals from retained, completed first-hit samples without retracing. An analytic inner-room test
checks improved projection agreement; broad, unsupported and outward planes cannot authorize shifts.
A bounded diagnostic budget planner now proposes additions or replacements from visibility-supported
receiver footprints. It preserves all currently covered samples, protects explicitly locked captures,
uses free layers before swaps and recomputes unique ownership after every change. The current report
uses the automatic pre-authored baseline and an experimental sampled-error gate, not a production
acceptance policy. Replan against final authored composition and qualify unsampled camera/dynamic
coverage before publishing any replacement; geometric baseline coverage is weaker than visibility.

Surface-only replacement trials exposed loss of airborne-prop reflections despite preserving every
sampled world surface. The diagnostic planner now adds zero-weight capture-origin and interior-grid
guards. They constrain spatial preservation without rewarding empty air as new surface coverage;
new local captures cannot claim unobserved guard visibility. This remains a sampled conservative
check, not full-volume preservation or unchanged radiance blending. Include removed-volume controls
alongside newly covered rooms in every replacement trial.

The coverage planner can now consume a metadata-only composed baseline that locks authored roles,
preserves rotated volume frames and maps back to original image layers. Empty composed sets retain
explicit disable semantics. This is CPU planning support; the current bake report remains automatic
before authored composition. Wire the composed snapshot into final-frame planning/publication and
qualify live authored edits before treating this as integrated replacement support. Bind the baseline
and plan to matching frozen-scene and authored-revision ownership before publication. CPU plan
contexts now own exact baseline records and record scene, chart, hint and authored revisions;
bitwise metadata comparison rejects changed planes, feathering, dimensions or source-layer mapping.
Wire that check into the frame activation path with retained image owners; the diagnostic's context
receipt alone does not make publication safe. Use published revision ownership, not pending edits.
Do not reuse a
layer-index plan after a composition or map change.

The prepared-image planner now produces an explicit retained-baseline/local-image copy recipe,
keeping authored IDs and precedence while sorting records, identities and copy sources together.
It checks prepared geometry against the owned candidate geometry and rejects stale source contexts,
protected replacements and incompatible image dimensions. Authored-only fallback policy is part of
the context and cannot silently gain automatic captures. This remains CPU preparation support.
GPU ImageAsset now accepts this explicit selection recipe, checks both source headers before
recording, and shares the existing queue barriers and mip-copy implementation. The caller must
retain both source image owners through the copy fence. This does not yet activate replacements.
SelectionUpload now owns both source images and the prepared output, checks context before
recording, and checks it again when the caller retires the copy after its frame fence. Stale
unsubmitted work records no commands; stale submitted work keeps its owners until retirement and
produces no publication. The returned publication bundles image, identities and context. The frame
controller must retain the transaction until its fence and check context again if activation is delayed.
SelectionFrames now retains submitted copies and sampled publications by fenced frame slot,
retires copies only after selectFrame, and uses offer generations to prevent older completed work
from replacing newer requests. Context is rechecked during record before activation. This frame
handoff is connected only under the diagnostic local-selection request described below.
Completed optional receiver diagnostics now retain compact fitted-volume and classified-visibility
evidence on the published bake volume. Query rays, transport and readback storage are released as
before. The planner can reuse this evidence against a fresh baseline only when frozen-scene
metadata still matches; unknown observations remain unknown. Cached bakes currently have no such
evidence, and ordinary baking without the diagnostic request does not allocate it.
LocalCaptureWork now prepares at most four selected captures from the owned candidate records,
validates source ownership and destination protection before allocating radiance, and uses existing
CaptureWork/filter aliasing so same-origin shapes share rays and mip filtering. Fenced readbacks
are range-checked, duplicate-checked and validated atomically; finish produces filtered pixels and
candidate-to-layer metadata together. Transport dispatch and renderer scheduling are opt-in as described below; this CPU component
does not activate local captures on its own.
REMIX_DEBUG_PROBE_LOCAL_CAPTURE=1 now schedules selected local radiance after ordinary bake
filtering, using the same frozen transport, configured samples/bounces, adaptive GPU budget and
fenced readback slots. A preparation worker filters and creates the local image; bake activation
uploads it and retains its plan/ready metadata separately on the published volume. Its staging is
released only at the upload fence and its allocation is included in memory statistics. This requires
fresh retained diagnostic evidence; cached bakes skip it. The local image is not yet selected by
rendering, so enabling this request alone changes bake work but not reflection appearance.
REMIX_DEBUG_PROBE_LOCAL_SELECTION=1 now connects the opt-in prepared captures to renderer
publication. The renderer snapshots the actually published image, authored IDs/roles and revision,
restricts replanning to available prepared candidates before greedy selection, and checks the local
image against the same frozen scene and approved geometry. Source image owners survive the copy
fence; SelectionFrames publishes descriptors, header and IDs together after retirement. World,
viewmodel receiver masks and explicit authored receiver bindings use the same active publication.
Changes to source image ownership or baseline context invalidate pending/active selections while
submitted and sampled frames retain their owners. Output image memory is included in statistics.
LocalSelectionPreparation now runs planning and image allocation on the shared preparation queue,
retaining immutable inputs and polling without waiting on the render thread. Superseded queued
work is cancelled immediately, running work receives cancellation, and finished work must still
match current scene metadata and both source-image owners before entering the copy queue. Cached bakes
still have no retained evidence or prepared local captures.
The opt-in route has now been exercised with full-resolution SSR and moving camera fixtures on
Trainstation and Construct, including reflective props in added regions and retained-volume controls.
Construct's authored image records remain protected during automatic replacement. These are static
BSP relay fixtures, not natural player routes or full-map/gameplay acceptance. Low surface-bake
settings in these checks do not qualify existing lightmap artifacts or indirect-light quality.
The underwater Construct receiver near (827, 4256, -875) still shows a sharp reflected region with
SSR disabled. Its prepared local image contains finite-depth black regions next to lit blue regions;
compare capture transport lighting/material/medium handling with the path tracer before assuming
this is a sampling seam or smoothing it away. The local-only dump request
REMIX_DEBUG_PROBE_LOCAL_IMAGE names a fresh serialized Data file (header plus filtered RGBA32F
mips, including unfiltered first-hit distance in base-mip alpha). It allocates/writes only when requested
and avoids a whole-map raw capture dump. Keep this underwater case open before visual acceptance.
A controlled one-versus-four-bounce follow-up on the same merged runtime separates capture
illumination from projection quality. The selected local image retains bit-identical metadata and
first-hit distances; 79,005 of capture zero's 79,265 initially black base texels gain nonzero indirect
light. The previous one-bounce diagnostic therefore cannot qualify dark cubemap illumination.
However, the underwater reflective prop still has the sharp region at four bounces with SSR disabled;
increased indirect depth is not a demonstrated projection fix. Both synthetic moving-camera runs
finish, preserve the authored sidecar, and report no validation errors. Keep production's three-bounce
default unchanged. Compare this remaining region against path-traced reflection geometry and the
other containing capture before altering filtering, box fits or blending. These low-atlas, OIDN-off
fixtures do not establish final lightmap quality or three-bounce production visual acceptance.
An accumulated, TAA-off path-traced control at the same underwater camera/material reflects the
nearby pale floor instead of the strongly blue projected shape. It completes at four bounces and
353 accumulation frames; the installed validation layer emits timeline-semaphore state warnings,
so this is visual diagnostic evidence, not a clean validation result. Baked radiance already carries
Beer-Lambert loss from the capture origin. Reusing it at a distant underwater receiver changes the
physical attenuation path: investigate capture/receiver medium and path-length differences as well
as box geometry. More blur or a larger diffuse bounce budget is not an established remedy.

A separate outside-proxy sampling defect is fixed in the CPU reference and shared GPU sampler.
When influence extends beyond projection, parallel-slab and disjoint-interval misses now retain the
unprojected direction instead of inventing a far-face hit. Entering rays still use the actual far face,
and interior receivers retain their original shader path. Analytical miss/entry cases and translated,
rotated GPU comparisons across roughness mips pass, along with authored edits and viewmodel SSR/
environment readback. This does not establish a fix for the underwater receiver inside its proxy.
Shared-floor priority blending now has an analytical continuity regression across differing
feather widths. Previously a 15.625% doorway layer could become fully opaque immediately above
a common floor because the two volumes scaled that floor band differently. Derived per-face bands
now use the same width for exactly coincident outward world planes, including permuted local axes;
spatial blend weights and canonical records remain unchanged. The 2 KiB GPU metadata extension is
rebuilt for publication and does not alter disk-cache or sidecar formats. CPU analytical cases,
rotated GPU sampling near the floor, finite HDR energy, uploads/composition/selection, cache integrity
and viewmodel regressions pass. A low-atlas Construct synthetic route completes without validation
errors and preserves diffuse hashes, authored statistics and the saved sidecar. Its checked main-room
and underwater frames remain unchanged; connector changes are small. The comparison also includes
the preceding outside-proxy fix, so it does not isolate priority-band image differences or establish
whole-map visual/performance acceptance. Underwater medium/path-length work remains open.
Completed-worker local-selection lifecycle coverage now complements queued cancellation tests.
A synthetic frozen-chart fixture supplies completed fit/visibility evidence, prepares an actual output
image, records its source copies and checks publication only after the submission fence. Scene and
authored-context edits, equal-metadata image-owner replacements and cancellation after preparation
must discard the output. Prepared local metadata is now compared with its retained image on the
worker, refusing mismatched source metadata before output allocation or render-thread recording.
This closes an isolated transaction/ownership gap, not live edited-map or map-transition acceptance;
that integration and cached preparation remain open. Lifecycle cases live separately from generic
image upload tests to keep their setup and synchronization requirements readable.
Next: qualify live authored edits/map transitions, underwater capture lighting, and cached-bake
preparation, then consider local replacement publication as a default.

Before production integration, keep planner indices tied to canonical capture metadata rather than
sorted GPU layers, replan against protected authored composition and validate both added rooms and
regions served by removed captures. Preserve frame-publication ownership, diffuse bake identity and
exact-ray radiance reuse. Reflection selection/cache revisioning must distinguish changed plans
without needlessly invalidating surface/probe lighting.

Cross-map qualification must include local fits that improve yet retain large absolute error.
Do not convert improvement alone into automatic acceptance: receiver-supported origins can still
produce poor boxes, especially in the deliberately uncovered regions targeted by this diagnostic.
Require a useful final projection, visible transitions and preserved baseline coverage within budget.
Use absolute relative residual alongside miss/backface fractions for refinement acceptance. The
existing agreement score counts nearer geometry as supported, so it is not a symmetric accuracy
measure and must not alone reject a tighter fit or accept a loose box.
Fixed views over low-detail reflected terrain are weak parallax acceptance evidence. Use clear
reflected landmarks and moving receivers/cameras before accepting refined shapes, then repeat
across maps with frozen lighting and capture/influence identity controls.
Next compare reassessed bounds and visual parallax, including terrain
and dielectric interfaces, before automatic publication. Near-hit statistics can expose a floor
plane well behind actual terrain even when a candidate provides useful receiver coverage. Investigate
bounded, robust surface-plane fitting with independent receiver influence, keeping capture origins
inside the projection and preserving partition occupancy. Do not move faces to whole first-hit AABB
envelopes or discard near hits as clutter without evidence. Then check moving receivers, overlap
transitions, leakage and renderer cost across maps. Large underwater boxes must not win solely for
geometric area or sampled visibility; neither establishes parallax accuracy.

### Optional local reflection cache continuation

Prepared local captures and compact receiver/fit evidence can now be persisted in an
optional `.local-reflections-v1` companion to the ordinary lighting cache. The base
cache format and diffuse identity remain unchanged. Warm preparation verifies the
lighting key, content, receiver ordering, capture mapping and fitting policy before
rebinding runtime generations and uploading the image through existing fenced owners.
Missing or rejected companion files retain the ordinary lighting-cache hit. Both local
capture and local selection requests remain opt-in; this is not a default promotion.
CPU checks cover byte-exact round trips, generation rebinding, geometry/policy changes,
invalid ownership, corruption, truncation and cancellation. Persistence admission now
includes the retained local image and evidence. GPU upload/lifecycle checks pass with
synchronization validation. An isolated four-bounce Construct cold/warm route restores
four local captures without their transport rays on the warm run, with identical bake
identity, content hashes, probe records and authored statistics; exposure differs.
These are synthetic relay checks, not gameplay performance or visual acceptance.
Legacy caches lacking the companion still cannot prepare local improvements without
fresh evidence. Live edits/map transitions, underwater medium/path-length behavior,
wider-map quality and default acceptance remain open. Earlier cached-path limitations
above describe the state before this continuation.
Corrupted-companion replay also preserves the base bake identity/content hashes and
performs no local transport; checksum rejection falls back without a full bake.

A cached Construct live-sidecar continuation now changes authored priority and independent
feathering after both initial authored publication and local selection are active. The
production watcher publishes revision 2, reuses both authored captures with zero new
capture rays, and replans the four local replacements. Final records show the requested
priority 2 and 96-Source-unit feather band; bake identity/content hashes and the original
user sidecar remain unchanged. The isolated relay exits successfully without validation
errors or synchronization hazards. A separate edit during initial authored preparation
also reaches the latest revision. These functional routes do not qualify all edit types,
visual transitions or gameplay performance. GPU lifecycle regressions now additionally
reject a completed worker after a scene-ID change, and reject submitted old-map output
after its fence while retaining/releasing source images correctly. Actual in-process
map switching, deletion/empty sets, capture-origin moves and underwater quality remain
open before default publication acceptance.

Cached Construct live-sidecar empty/removal checks now cover two more production
watcher transitions after authored and local publications are active. An explicit empty
set publishes zero local probes at revision 2. Removing the owned sidecar restores 64
automatic/local probes with no authored identities. Both retain the same diffuse bake
identity/content hashes and perform zero capture rays; the protected user sidecar stays
byte-identical. Final-renderer repeats exit without validation errors or synchronization
hazards. The renderer now skips local-selection preparation entirely for explicitly
disabled or author-only policies, while retaining fenced retirement of existing work.
Worker regressions cover both policies without output-image allocation; the authored
edit/reuse and GPU image/lifecycle suites pass. These checks close these specific
functional transitions, not pixel-level doorway quality or actual in-process map changes.
Capture-origin edits, map switching, legacy-cache preparation and underwater reflections
remain open before default publication acceptance.

An underwater fixed-box origin control now compares a protected 128px capture at
(1826.47, 4645.57, -268.03) with one at the reflective receiver (827, 4256, -875).
Projection/influence boxes, priority 10, roughness .15, metallic 1, four diffuse bounces,
SSR-off policy and camera route are held fixed. Both runs preserve diffuse identity and
content hashes, publish the intended capture, and complete without validation errors
or synchronization hazards. The near capture removes the prominent angular split and
shows a more plausible floor-shaped reflection, but remains bluer than the accumulated
path-traced control. Moving the origin changes optical path lengths and proxy projection
error together; this does not isolate absorption, prove the projection box correct, or
justify removing attenuation. Do not blur away the difference or promote this hand-placed
control as an automatic placement fix. Investigate receiver-near admitted origins and
terrain/medium-aware transport parity, then repeat moving-receiver and boundary checks.

The near-underwater candidate was already admitted and fitted at approximately
(827.58, 4256.26, -879.46), but its best original weighted absolute projection error
is 0.342, above the unchanged 0.20 selection gate. Its proxy floor at -959.96 lies
below terrain whose retained 10th/50th/90th hit coordinates are approximately
-916.76/-908.78/-901.24. Strict relative-only refinement remains near 0.341.
Optional diagnostic refinement now also reports a separately labeled scene-unit
uncertainty budget derived from the configured detail tolerance. With the existing
0.5 m budget, inward floor contraction to -908.74 and top contraction to -160.04
passes occupancy and retained-geometry reassessment, reducing this candidate's error
to 0.101. The original proposal/control remains in the report. Geometry audit rays
remain 12,288 and the coverage-selection plan remains identical: no candidate is yet
published from this alternative. CPU regressions check the absolute bound, support,
interior origin and no outward growth; the fresh Construct relay completes without
validation errors or synchronization hazards. A sampled proposal is not a certified
terrain plane. Next integrate only independently reassessed admissible variants within
the existing per-site/capture budget, preserve receiver influence and authored roles,
and qualify reflected landmarks, moving receivers, leakage and underwater colour.

ReceiverFitAudit can now add inward-refined, independently reassessed alternatives to
completed local fits. Originals remain in their original order; projection/influence
duplicates are removed, retained receiver influence is unchanged, and only improving
occupancy-checked alternatives fill unused slots up to the existing six-per-site cap.
The visibility planner, report and retained evidence consume the same deterministic
result. Hardware geometry rays are reused; the four-local-capture and 0.20 error limits
remain unchanged. Analytical tests cover retained originals/influence, contraction,
occupancy, full-capacity refusal and map-change cancellation. Geometry, cache and GPU
image/lifecycle suites pass.
A fresh four-bounce Construct route now selects the receiver-near underwater refinement
at error 0.101 within its four replacements. The angular reflection split disappears
on the checked prop, similar to the hand-placed near-origin control; underwater colour
still differs from path tracing. The 12,288 geometry rays, base lighting hashes and
authored-capture statistics match the control. Cold/warm cache routes publish identical
probe records/content and authored statistics apart from exposure, with four local
captures restored and zero local transport rays on the warm run. Runs complete without
validation errors or synchronization hazards; the original user sidecar stays unchanged.
These low-atlas, OIDN-off synthetic routes do not qualify final lightmaps, all reflected
landmarks, doorway leakage or gameplay performance. The feature remains opt-in, older
companions remain usable without gaining newly refined captures, and legacy-cache
preparation, actual map switching and broader quality/default gates remain open.

A four-bounce d1_trainstation_02 before/after moving-camera relay with full-resolution
SSR now checks the refined-fit integration outside Construct. Both builds retain the
same two selected replacements, gained-area estimates, diffuse identity/content hashes
and 12,288 local geometry audit rays. Both exit without validation errors or sync hazards.
Eight of thirteen captured frames are pixel-identical; five have localized differences
(121-331 pixels, maximum display-channel differences 38-76), so this is not whole-route
pixel identity. The inspected interior corridor is unchanged. Some outdoor route views
expose relay geometry limitations and cannot establish gameplay visual quality. Keep
moving gameplay, reflected landmarks, doorway/wall leakage and wider-map acceptance open;
this check supports bounded non-regression on this fixture, not default promotion.
The renderer guide now documents the experimental flags, refined-variant constraints,
companion-cache behavior and publication ownership in one current section.

Legacy lighting-cache hits can now prepare a missing local companion under the existing
explicit local-capture and fresh receiver-diagnostic requests. The cached path creates
only the transport needed for bounded admission/connectivity/geometry checks and local
captures; grid, surface, direct visibility, sun and probe-depth work remain skipped.
The existing fenced upload/publication path consumes the completed output. Persistence
uses an explicit companion-only CPU payload, bounded by the same queue admission and
memory accounting, without moving or rewriting the base lighting payload.
A Construct cache copied without its local companion upgrades to four local captures;
GPU work counters report zero points/batches for grid, surface, visibility, sun and
probe depth. The base bake file is byte-identical afterward, and the companion is
byte-identical to the independently generated fresh-bake companion. A subsequent load
without receiver diagnostics restores all four captures with no local transport rays.
Fresh/upgrade/warm reports agree except exposure and retain original authored data.
Both relay runs complete without validation errors or synchronization hazards. Cache,
background persistence and GPU image/lifecycle suites pass; the cache test independently
proves a companion-only background write preserves an existing base file. Report-independent
preparation, failed optional transport handling, map switching, wider quality/performance
and default promotion remain open. This is an opt-in upgrade, not a diffuse rebake or an
installed/default change; earlier legacy-cache limitations are historical.

A normal-default Construct quality route now complements the cheap functional fixtures:
1920x1080, 4x MSAA/full-resolution SSR, 256 bake samples, three bounces, 32,768 probe
slots, 4,194,304 target surface texels and OIDN enabled. The actual bake contains
3,837,562 surface texels across four 1024px pages at about 0.24 m spacing. OIDN reports
6,483 charts and 4,059,932 processed texels (including chart preparation), 13.917 s;
these are one synthetic-run diagnostics, not uncontended throughput acceptance.
The inspected main-room and connector/ceiling captures are substantially cleaner than
the 64-sample, quarter-million-texel, OIDN-off fixtures. Existing material highlights and
all seam/lightmap quality gates still require matched-reference review. This normal route
selects a different underwater capture: the receiver-near candidate that improved the
cheap fixture is absent from the final set, and an angular blue reflected region remains.
Do not describe the earlier local-fixture improvement as a normal-quality underwater fix.
Investigate sampling/baseline/coverage ranking and preserve normal-quality controls before
promoting automatic selection. Both the dense bake and OIDN execute; the run exits without
validation errors/synchronization hazards and preserves the original authored sidecar.
Future visual acceptance should use labeled normal-quality captures with recorded runtime
hashes/settings; low-atlas transport fixtures remain useful only for their narrower gates.

Normal-quality underwater selection diagnosis narrows the remaining failure. A nearby
candidate at approximately (950.45, 4559.11, -896.92) passes admission/connectivity and
has a reassessed variant at error 0.0507, below 0.20; it is not rejected for geometry.
Its observed visible footprint contains 267 receiver samples, versus 282 for the selected
far origin (1826.47, 4645.57, -268.03). Their footprints overlap at 263 samples, and only
four of the near candidate's samples remain uncovered after the final selection. These
are classified sampled footprints, not a full visibility proof. The greedy coverage
objective therefore favors the far capture before the near alternative can add useful
new area. Projection error only breaks equal-gain ties; globally weighted proxy error
also favors the wall-adjacent far origin (0.0184 versus 0.0507), despite the reflected
underwater result. Mean visible connection distances are similar (about 40.9 m far and
40.4 m near), so an unqualified nearest-origin distance heuristic is not established.
Next test a bounded receiver/medium-aware fidelity objective or localized influence
representation while preserving baseline guards, authored priority and the capture budget.
Do not loosen admission/error thresholds, increase samples or claim that extra captures
alone resolve this mismatch. Keep normal-quality and path-traced controls in acceptance.

An offline receiver-space proxy comparison now checks the same 263 observed-visible
receivers for the normal-quality underwater far/near alternatives. Against the sampled
receiver endpoints, near has a lower median spatial residual (0.00238 m versus 0.0619 m)
and wins at 152 receivers, but its mean is worse (7.12 m versus 1.71 m), with a 24.7 m
90th percentile and a 48.7 m maximum. A global median score would hide this tail; these
measurements do not justify swapping to the near proxy throughout its full influence.
The maintained stdlib diagnostic exports per-receiver coordinates/errors and common-set
summaries from matching reports, without GPU work or renderer policy changes. Analytical
axis/diagonal/translated-origin and undefined-direction cases pass. Use its spatial rows
to test localized influence or a regional fidelity objective while preserving existing
coverage/guard and authored constraints. Underwater radiance/medium parity and normal-quality
acceptance remain open; global score replacement and default promotion are not established.

Normal-quality authored controls now test local influence separately from global proxy
ranking. At 256 samples/three bounces/OIDN/4M texel budget and SSR disabled, a near pool
capture at the admitted origin (950.45,4559.11,-896.92) with restricted influence improves
the checked prop while the base diffuse hashes remain unchanged. This does not fit the
influence automatically. An initial moving control disabled local replacements and lost
the useful broad pool fallback; it is not accepted as a blend-isolation result.
A corrected paired route includes the same explicit broad capture in both cases and adds
only a priority-10 near overlay with influence [700,4100,-960]..[1100,4700,-760] and a
64-Source-unit feather band. Automatic replacements remain disabled for isolation. A
camera-relative reflective receiver moves from X=650 through the region to X=1150. The
interior sphere ROI matches baseline exactly at the two exterior controls on each side;
mean absolute display-channel differences grow through the entering band (0.94,5.01,6.71)
and decline through the leaving band (6.41,4.99,1.12), then return to zero. These sampled
frames support a smooth handoff in this fixture, not every continuous path or surface.
The automatic set remains 59 captures with zero omissions, existing authored records
match, and total layers rise from 62 to 63 within the 64-layer cap. Diffuse identity and
content hashes remain unchanged. Both corrected runs reuse normal-quality cached lighting,
exit without validation errors/synchronization hazards and preserve the original user
sidecar. Localized influence is therefore a promising controlled representation; the
bounds are hand-selected and not installed. Next derive bounded regional support from
suitable evidence (including already-covered receivers), then qualify coverage, leakage,
medium/radiance parity and capture allocation. Do not substitute the incomplete uncovered
receiver cohort or a global median score for an automatic fit.

Experimental regional evidence preparation now addresses the audit's uncovered-only
blind spot. A separate explicit request reserves at most 8,192 already-covered
receiver/origin checks within the same 32,768 total ceiling; the default marginal cohort
is unchanged. Local reports carry receiver positions so comparisons no longer require
them to appear in the original uncovered audit. CPU checks cover rotated existing
coverage, mixed/cohort budget sharing and invalid reservations. Older reports remain
readable by the offline comparison tool.
A normal-quality cached Construct upgrade executes 18,186 local pairs: the original
9,994 marginal observations are identical and precede 8,192 new regional checks.
Receiver 7692 near the reflective prop is now explicitly classified visible to both
pool candidates, rather than being absent from local evidence. The two candidates have
309 common observed-visible receivers in this run; their enlarged residual distributions
still expose the near proxy's poor tail. The plan is unchanged on this fixture, the
base lighting cache stays byte-identical, and the original sidecar is preserved. The
relay completes without validation errors or synchronization hazards. Preparation uses
an isolated hard link to the already-built normal cache to avoid duplicating its large
payload; only the new local companion/report belongs to this run. Regional evidence is
a foundation for bounded influence support, not a new automatic selection policy or
normal-quality visual acceptance. Do not infer arbitrary unsampled volume support from
these classified surface connections.


### Probe support sharing: next qualification

The fixed Construct ceiling-housing study supports investigating shared opaque
connection visibility while retaining each receiver's spatial interpolation weights.
A constant SH value for the whole draw is not an accepted replacement: across the
same anchor cohort it can worsen error, including when removing the normal-facing
weight. One anchor has no clear support at all; missing support must remain unknown
rather than becoming black or being treated as unobstructed.

Next evaluate an independently geometry-derived anchor, without choosing a favorable
camera sample. A draw anchor is not a complete object anchor when an instance has
multiple submeshes. Check solid/interior anchors, missing stencil nodes, thin geometry,
rotation and movement before proposing a runtime cache. Preserve per-point weights
and distinguish opaque-only tests from alpha/transmission visibility. No shared-support
policy is enabled in normal rendering, and no performance benefit is established yet.

Visual acceptance must use the normal bake budget with denoising. Low-budget numerical
diagnostic replays are not visual-quality acceptance captures; their surrounding scene
may use reduced probes, texels and samples even when an independent reference query
uses a high sample count. Keep these purposes explicit in run metadata and reporting.


The first geometry-derived draw-midpoint check is rejected for the Construct ceiling
housing: its complete draw AABB centre lies in solid/unplayable BSP space. The
normal-quality cached replay preserves the original receiver estimates, but this
anchor yields no admissible support for the fixed receiver cohort. Do not bypass
containment or substitute a favorable camera pixel. Next investigate deterministic
exposed-surface anchors or multiple bounded support regions; validate the host partition
classification, receiver-side connectivity and submesh ownership before runtime use.
An embedded fixture's geometric centre cannot be assumed to represent room lighting.


A bounded exposed-surface diagnostic now ranks complete playable triangles by area
within six signed dominant-normal directions. It aligns geometric normals with vertex
normals, offsets centroids outward by 2 mm, rejects unknown/solid partition positions,
and excludes only the source draw from opaque blockers. This is an opt-in report, not
normal shading. CPU checks cover independent centroids and rejection of solid anchors.
The first normal-quality cached Construct check finds useful surface candidates but also
finds missing target probe IDs for some candidates in the same playable cluster. Do not
classify missing IDs as clear, compare partial cohorts as complete, or choose an anchor
solely by the lowest observed error. Next audit the union of target stencil nodes at each
candidate, preserve target interpolation weights, and test receiver-side connectivity,
multiple submeshes, moving objects and other maps before considering runtime support.


The surface-anchor report now audits the fixed union of receiver stencil nodes at
every candidate, separately from its own local stencil. Work is bounded by the existing
sixteen-ray request and sixteen nodes per receiver, in batches of sixteen connections.
The Construct housing comparison no longer loses coverage at candidates whose local
stencils omitted required nodes: every admitted candidate supplies known support for
the whole checked cohort, with lower error than the original interpolation in this
fixture. Original receiver estimates remain unchanged. This supports further shared-mask
research, not a production selection rule. Candidate disagreement persists; qualify
support-region connectivity and motion before choosing a cache lifetime or runtime policy.
Keep full target-node unions when comparing anchors, and retain missing/untested states.


Surface-anchor diagnostics now report candidate-to-receiver connectivity independently
of candidate-to-probe masks. Receiver endpoints use 2 mm offsets along the picked
geometric normal; closed-partition interval traversal and eligible opaque blockers are
reported separately. Missing/too-short links are absent, not implicitly clear. The
CPU thin-slab fixture rejects a solid crossing even with playable endpoints. All admitted
Construct housing candidates connect clearly to the fixed receiver cohort, so this guard
does not resolve their differing probe masks in that case. Do not use connectivity alone
as an anchor ranking metric or claim it proves lighting fidelity. Stable selection,
visibility-transition behavior, instance ownership and broader quality/cost qualification
remain open; the normal renderer still uses the existing probe reconstruction.


Offline equal-vote consensus is a promising follow-up to surface-anchor selection.
For the fixed housing cohort, majority visibility preserves complete support and improves
reference agreement without selecting a favorable anchor. Requiring unanimous clear
votes removes all support; accepting any clear vote retains more leakage. All three- and
four-candidate subsets reproduce the full majority result in this fixture. Unknown votes,
empty candidate sets and tied votes remain unknown, not fabricated visibility. This is
not motion stability or multi-map acceptance: correlated candidates can still agree on
an incorrect result. Broaden to other prop geometries and disconnected/concave surfaces,
then qualify an engine-agnostic runtime representation and cost before enabling any
consensus policy. No normal-renderer visibility voting has been introduced.


The first broader Construct check rejects equal-vote surface-anchor consensus as a
general reconstruction policy. Sixteen model-surface sites across fifteen actual draws
use the normal cached grid and independent base-only 8192-sample hemisphere references.
One intended draw shares its exact tested position with another draw; actual ray ownership
is retained. Consensus loses support at four sites (ties or no connected candidate), and
its error is slightly worse on the twelve common covered sites, despite improvements
on several fixtures. Some strongly underlit sites remain far from the reference under
both methods. Do not promote the housing-only success, hide missing sites, relax ties
or use a selective cohort to claim a global improvement. Keep consensus offline; next
separate grid transport/placement errors from receiver interpolation and anchor failures
at the regressing sites. The original renderer remains unchanged.


Independent hemisphere checks at the strongest contributing probe origins now distinguish
field accuracy from receiver-space variation for three large-error model sites. At the
two severely underlit surfaces, contributing probe positions remain genuinely much dimmer
than their surface references; correcting stored coefficients alone cannot recover that
near-surface energy. At the overlit surface, strongly weighted probe positions genuinely
receive bright lighting even though its surface reference is darker. Some stored-versus-
reference probe errors also remain (sampling and SH truncation are not separated here).
This is evidence for inadequate spatial representation around sharp lighting changes,
not a reason to globally raise samples or zero visibility weights. Next test localized
receiver-aware allocation/placement while protecting thin-surface containment and the
fixed global probe budget; prior placement/refinement regressions remain applicable.
Run metadata now explicitly separates reduced surrounding replay quality from independent
8192-sample reference queries. These diagnostic images are not visual-quality acceptance.


Relocation now constrains receiver-normal clearance to the first closed-partition solid
interval. Previously a long offset could cross a thin wall/ceiling and end in playable
space on the wrong side. The shared CPU helper also serves the existing diagnostic;
unknown/already-solid starts remain inadmissible. A thin-slab regression covers playable
endpoints separated by solid space. Bake algorithm revision 35 prevents reuse of old
placement results. Grid, adaptive-grid, cache and receiver-diagnostic tests pass.
A fresh normal-budget Construct bake retains the full checked receiver coverage and
stays within the original probe record budget. Most checked estimates are identical;
one is slightly worse, and the large model errors remain. This is a containment fix,
not a demonstrated visual-quality improvement or deployment qualification. Check the
previous ceiling fixture, additional maps and cache reload before installing it; receiver
binning and refinement priorities still use their existing policies.


Revision-35 Construct cache reload now completes with zero grid, surface and visibility
transport work. The original sixteen ceiling-housing receiver estimates match the prior
field exactly, retaining all coverage; the containment change does not fix that fixture.
The protected authored cubemap sidecar remains unchanged. Allocation inspection confirms
that normal refinement ranks charted lightmap receivers; uncharted model geometry has
no independent receiver-error references in that path. Investigate bounded model evidence
separately from increasing the surface atlas budget, while retaining the documented
rejection of earlier model-driven refinement that moved nodes through the ceiling.
Containment alone does not establish that repeating that policy will succeed.


Refinement receiver binning now uses the same contained clearance as relocation,
preventing a receiver near a thin slab from allocating its patch across the slab.
A cell-ownership regression checks the actual adaptive directory, and adaptive/cache
CPU tests pass. Candidate cache revision 36 isolates this changed allocation policy.
A fresh normal-budget Construct bake preserves checked coverage and the global record
budget but changes four model-site estimates, with mixed improvements/regressions and
slightly higher cohort RMS. Single-run planning time is similar in scale to revision 35;
this is not a bake-speed improvement claim. Keep the candidate undeployed while studying
which allocations changed and guarding broader quality. Correct topological assignment
does not itself establish better sampling distribution or lighting fidelity.


A same-budget spacing sensitivity check rejects changing the default request from 2 m
to 0.5 m as a quality fix. Under the same 32,768-record ceiling, budget fitting grows
the coarse allocation substantially and leaves fewer records for fine levels. Actual
finest allocation becomes coarser than baseline, despite the smaller requested setting.
The fixed sixteen-site model cohort retains coverage but worsens overall error, with
more regressions than improvements. Keep the original setting. Next decouple coarse
coverage from local refinement demand and evaluate receiver-aware marginal allocation;
the current bounded directory supports only 2x/4x/8x cell subdivisions, so truly small
local support may require a deliberate representation change with CPU/GPU/cache parity.
Do not silently enlarge the record budget or describe requested spacing as achieved
spacing. Runtime/benchmark reporting must distinguish requested, fitted coarse and
finest allocated density; finest anywhere is not density at every receiver.


Local 0.25 m SH-cell feasibility checks do not resolve the selected high-error surfaces.
One cell has only one backface-valid corner; naive renormalization over that survivor
worsens its estimate. Another retains six valid corners but remains well below its
surface reference. These are simple trilinear feasibility estimates, not the complete
production visibility reconstruction, and cannot justify a denser regular-grid default.
Independent 8192-sample SH queries at the exact surface-reference positions closely
match hemisphere integration for both checked normals. This narrows these particular
failures toward spatial support/placement rather than inability of the stored SH format
to represent that directional irradiance. Next test geometry-aware local placement that
retains multiple usable, same-side contributors; guard against isolated surviving probes,
renormalization artifacts, thin solids and field discontinuities under movement.
Do not extrapolate the two-position SH comparison to arbitrary high-frequency lighting.


A surface-aligned 4 cm SH-stencil feasibility check separates the two difficult sites.
Normal offsets are 2..42 mm, with deterministic tangent axes and no post-trace relocation.
The underlit fixture's near-face interpolation approaches its independent surface
reference with complete near-face weight. The other site still loses half its near-face
weight to backface-invalid nodes and becomes overlit when the survivors are renormalized.
Exact-position SH has substantial backface incidence there, so receiver validity and
geometry-boundary/partition-side behavior need checking before allocating more density.
The cohort is diagnostic model-surface sampling, not a proof that every selected point
is visible in ordinary gameplay. Preserve that distinction without dropping difficult
sites from reports. Surface alignment alone is not an accepted general placement policy;
verify usable same-side contributors and boundary-aware fallback before runtime work.


Offline reference results now include each query point's captured BSP cluster, with
unknown and solid/unplayable states explicit. Classification does not relocate points,
filter results or alter transport. CPU output tests cover playable/solid points; a
repeated Construct local-cell query preserves all prior SH, moments and statistics.
It confirms that half the failed surface-aligned cell lies in BSP-solid space. The
other fixture also contains backface-invalid nodes classified playable by the BSP,
so model geometry and host partition tests remain complementary. Do not infer usable
probe support from either cluster membership or backface validity alone. Boundary
receiver transfer, sufficient same-side support and visible gameplay acceptance remain
open; this metadata is diagnostic only and changes neither cache identity nor shading.


A current-build two-view Construct replay repeat keeps selected-probe batching opt-in.
Opposite-order on/off pairs use identical cached normal-budget lighting, fixed hall and
white-room cameras, unchanged light budgets and full-resolution SSR. Completed timing
windows are untruncated; images and lighting content hashes match exactly. Batching is
slower on the paired means in both replay views, especially the room. This does not
invalidate the earlier live brush-shell result, but it blocks general default promotion.
Opaque shading/depth remain much larger than static shadow updates in these captures;
static shadow updates stay zero. Do not call the replay a live gameplay FPS test or
attribute the cost to light clustering merely because many captured lights exist.
Next isolate batch-helper shader cost and receiver eligibility while preserving live
probe-heavy controls, preparation churn/lifetimes and wider-map acceptance.


Pass-level review qualifies the latest batching replay slowdown: most of the room's
paired difference appears in depth-prepass time, while opaque-shading self time is
nearly unchanged. The hall delta is small and also includes depth variation. These
ranges do not isolate a batch-helper arithmetic regression; investigate cross-pass cache,
scheduling and run variation with hardware telemetry before changing reconstruction.
Source inspection also confirms selected slots are packed in runtime-light traversal
order and cluster lists preserve that order for unique captured lights. Do not add a
light reorder to address an unobserved grouping mismatch or alter accumulation order
without evidence. Keep the prototype opt-in and preserve the earlier live-room controls.


A reversed-order white-room replay repeat adds timestamp-window-aligned NVIDIA telemetry
and a longer warmup. Images, lighting identities and selected-light budgets remain
identical. Paired total GPU cost is effectively neutral; batching reduces opaque-shading
self time while depth time increases. Both modes stay in P0 with closely matched graphics
clocks, so a coarse clock-state difference does not explain this repeat. Individual runs
still vary; monitored fixed replay is not live gameplay/churn acceptance. Do not label
the helper intrinsically slower or promote it from one favorable pass. Next isolate
cross-pass cache/scheduling and receiver workload effects, preserving full-frame timing
and independent quality checks. Keep the prototype opt-in.


The prepass-off isolation retains exact images against the matched prepass-on white-room
captures. With aligned telemetry and unchanged cached lighting/selected budgets, batching
still has a small paired full-frame cost increase without the depth pass. This does not
identify a single cause for the earlier depth variation, and does not justify enabling
batching or disabling the prepass globally. Prepass-off shifts work into opaque shading;
other scenes, overdraw, moving/viewmodel receivers and live brush shells remain necessary
controls. Keep both current defaults. Avoid attributing inclusive opaque/depth timings to
independent costs or promoting a setting from sequential fixed-view batches alone.


### Official-main integration checkpoint

The feature branches incorporate official native main through `3d2b7f54` and adapter
main through `b89a651`. Forward+ and its GPU fixtures now follow upstream build-time
shader generation with pinned tools; generated headers/binaries are not committed.
Windows scratch cleanup, dependency escaping and manual output paths have regression
coverage. The combined path-tracing frame retains both IES and measured-sheet data,
with explicit uniform alignment and generated-SPIR-V offset checks. Legacy bake-statistics
prefix handling remains compatible; the stale layout assertion and Python audit DLL
search environment are corrected.

Selected CPU/ABI, UI, hardware transport/ownership, measured-sheet and input tests pass.
A reduced-budget two-backend adapter replay completes, but its first cold run reports
NVIDIA low-latency timeline diagnostics. A low-latency-disabled control is clean. Keep
that distinction: this is not blanket validation, visual-quality, gameplay performance
or deployment acceptance. Investigate the latency path with suitable validation/driver
controls before closing it. The playable install remains unchanged.

Upstream path-traced IES profiles, cookies and related light/media features are retained;
Forward+ parity is not established by merging them. Follow up with immutable capture,
cache identity, transport/shading and GPU quality coverage when adding those features.
The existing probe-quality, static-model, cubemap, live-game and bake-time gates remain.


### Projected textures and volumetric lighting after official-main merge

Official native main through `ea3f2965` and adapter main through `6543b6f`
are integrated into the feature branches. Animated projected-light cookies now
reach the shared light API; the shared adapter light creator preserves these
extensions, volumetric radiance scale 1, and existing surface-emitter baking hints.
Path tracing retains the IES/cookie slot fix and per-source volumetric scales;
its GPU frame includes both volumetric controls and measured-sheet optics.
These integrations do not establish Forward+ feature parity.

- [ ] Forward+ projected textures: carry immutable cookie texture identity,
  projection basis/FOV and frame changes into backend-owned light data and
  descriptors; apply colour-times-alpha masking consistently to diffuse/specular
  lighting and optional ray effects. Preserve cone fallback before upload.
- [ ] Keep moving/animated projected lights on the live path. If static cookies
  become bakeable, include their texture/content/projection in bake identity,
  transport and reflection captures without animation-triggered map rebakes.
- [ ] Forward+ volumetric fog: design a bounded, depth-composited scattering
  pass with shadowed sun/local lights, cookie support, temporal stability and
  camera-medium handling. Preserve per-light volumetric radiance scale and
  separate sun/light/emissive/sky controls; do not multiply surface lighting.
- [ ] Validate cookie orientation, outside-frustum darkness, alpha-only and
  animated cookies, texture arrival/replacement, shadowing, viewmodels and
  backend transitions. Profile volumetric memory/time and light-heavy maps
  before selecting defaults; retain the fully baked surface performance path.

Lighting quality, bake time and measured opaque-shading bottlenecks remain higher
priorities than enabling these additional effects by default.


### Viewmodel performance investigation

The current world depth prepass does not include viewmodels. Viewmodels render
after world/reflection work into an independent MSAA HDR/depth target to preserve
overlay ordering and world-space reflection guides. Eligible opaque weapon draws
already use early fragment tests, but have no dedicated depth-only prepass.

GPU diagnostics now separate viewmodel colour/attachment work from its local and
sun shadow passes; the existing overlay range remains inclusive. The adapter frame
CSV appends `viewmodelColorMs` without changing the public runtime API.

- [x] Qualify fixed-camera, no-prop, repeated unarmed/pistol/physgun blocks using
  foreground/status-gated samples and unchanged baked-lighting hashes. This
  establishes one Construct hall case, not all weapons/maps or PT parity.
- [ ] Evaluate a viewmodel-only depth prepass, preserving alpha coverage, equal
  depth ties, draw ordering, MSAA, SSR, world depth and isolated self shadows.
- [ ] Evaluate early weapon coverage rejection for world colour independently
  from reflection-guide depth. Do not write weapon depth into world SSR inputs.
- [ ] Study a supplied Source 2 capture's depth/stencil and pass ordering before
  claiming equivalent behaviour; a colour cutout alone is insufficient evidence.

The controlled hall run isolates a real equipped-weapon GPU increase while world
depth timing changes little. The viewmodel overlay includes substantial shadow
work as well as colour shading. Host intervals and native CPU recording also
increase; do not assume the hall is GPU-bound or attribute every FPS loss to
weapon pixel shading. Keep per-frame sums separate from sums of pass medians.
Severe user-reported drops and wider maps still require reproduction.


### Source 2 capture evidence for early viewmodel coverage

A supplied CS2 Vulkan capture replays successfully in RenderDoc 1.46. Its main
2560x1440 8x MSAA depth/stencil target receives world depth first, then weapon
depth before main world colour. Sampled viewport depth ranges are 0-0.95 for
world and 0-0.1 for weapon. The intermediate world colour export visibly leaves
the weapon silhouette unshaded. Sampled world colour draws also reject stencil
bit 0 when set. There is an intervening stencil clear/classification pass; do
not assume a weapon depth-pass stencil value survives directly into colour.

This confirms early coverage rejection in one real reference capture. It does
not establish all Source 2 backends, isolate its performance contribution or
explain downstream SSR/depth reconstruction. Qualify weapon depth and world
coverage rejection independently, keeping our world reflection guides correct.
Numeric depth ranges depend on projection conventions and are not portable
constants to copy. The second supplied capture fails file parsing.


### Viewmodel depth prepass prototype and acceptance gates

A private `REMIX_DEBUG_VIEWMODEL_DEPTH_PREPASS=1` ablation now writes matching
alpha/backface depth into the independent weapon target before colour. Colour
depth is read-only only when the depth pipeline exists. Shared draw submission
preserves ordering and transforms; world depth, SSR guides and isolated shadows
remain unchanged. Default stays off. GPU scopes distinguish target, depth and
colour; frame CSV appends `viewmodelDepthMs`.

Five final-source GPU regressions pass with synchronization validation, including
all alpha comparisons in both submission orders and 1x/2x/4x MSAA, weapon shadows,
probe inheritance, SSR and environment response. All 156 deterministic overlay
fixture readbacks exactly match the baseline. A real foreground Construct run
confirms the prepass executes on pistol and physgun with unchanged baked content.
Colour shading is cheaper in this hall, but world/shadow and whole-frame timings
vary enough that no default FPS improvement is accepted. Keep the opt-in private
until wider maps/weapons and repeatable whole-frame measurements justify it.

An initial wiring error enabled read-only colour without creating depth; the
broader alpha-order test and a small RenderDoc repro caught it. Those earlier
no-prepass runs are not optimization evidence. Temporary capture hooks and
unhelpful barrier/shader experiments were removed. World coverage rejection is
still a separate pending implementation; shadow cost remains another target.


### Opaque shadow alpha work reduction

Sun and local shadow shaders now skip albedo sampling/composition for ALWAYS
alpha and discard NEVER immediately. Conditional alpha retains the original
sampling/comparison; radial local depth, caster ownership, bias and filtering
are unchanged. The generated shader control flow confirms the opaque bypass.
No new setting or duplicate old implementation is retained.

Targeted GPU regressions pass with synchronization validation for viewmodel
shadows/MSAA, skinned shadows, mixed local maps, planar point-light self shadows
and baked sunlight/moving casters. Existing 156 deterministic overlay images
remain exact; 48 additional shadow-alpha readbacks verify rejected and passing
conditional world/self casters.

A fixed-sky, cache-hit, foreground Construct session pair has lower measured
weapon sun-shadow and overlay cost. World/unarmed timings also vary, so no
whole-frame FPS gain is accepted from that pair alone. Small live image
differences remain distinct from exact deterministic fixture parity. Wider
maps and repeatable timing remain open; maps still rebuild every frame. Audit
complete input stability before attempting shadow reuse. A fragment-free opaque
sun depth pipeline and world colour coverage rejection remain possible next steps.


### Playable static-model default and shadow refresh

The testing install is refreshed from `forwardplus-static-models-20261025`,
native `20d5f30f` and adapter `b04b344c`. All 157 owned files verify; 101 current
protected settings, launchers, authored cubemap bounds and replacement files
remain byte-identical. Only proxy/native DLLs and the runtime manifest changed.
The updater points to the new immutable package.

Installed normal-quality Construct with actual frozen sky and no model-chart
override generates 1407 model charts, loads compatible baked lighting and
completes image/report publication without validation/synchronization diagnostics.
Its fixed-view output matches the qualified reference exactly. A path-tracer
measured-sheet GPU regression is also clean on the build runtime, without
claiming installed RR coverage. The experimental weapon depth prepass and
selected-probe batching remain off unless explicitly enabled.

This qualifies the deployment and one fixed view; it does not close gameplay FPS,
first-bake cost, dynamic probe support, seams/noise, cubemap placement, wider-map
quality or the existing RR synchronization investigation.


### Vertex-only opaque sun trial: rejected and removed

A vertex-only sun-shadow prototype preserved conditional alpha, local radial
depth and camera-independent caster membership. GPU regressions and exact 204
fixture images passed; a small RenderDoc capture confirms six actual shadow
draws without a fragment stage. A null-module/GI layout classification issue was
found and corrected in the trial before those checks.

Foreground fixed-sky/cache-hit Construct off/on/off sessions retain identical
runtime binaries and baked content. Apparent first-pair improvements do not
survive the repeat control: the repeat off run is faster in both equipped-weapon
whole-GPU cases, and the on-run physgun sun range is worse than both controls.
World/unarmed variation remains substantial. This evidence does not prove a
repeatable benefit or a universal hardware result.

The prototype, extra flag/pipeline/command field, binding helper and temporary
capture hooks are removed from production. The local patch/evidence is retained
for future isolated testing. The new camera-policy shadow regression remains;
four restored-path GPU regressions pass and all 228 expanded images agree.
Playable install/updater remain unchanged. Prioritize the measured white-room
opaque receiver/field bottleneck before further small shadow-path experiments.
Audit exact-zero response pruning carefully: finite-emitter SH and fallback/mixed
corrections prevent naive shading-normal or cone rejection.


### White-room receiver cost: narrowed by diagnostic ablation

Foreground fixed-camera Construct trials with identical normal-quality baked
content and the same binary place white-room opaque shading excluding depth at
7.99/7.92 ms normally versus 1.75/1.76 ms when per-light response is omitted.
This removes visible lighting and is diagnostic only. Styled surface omission
leaves that cost near 8 ms; cluster building costs roughly 0.02-0.04 ms.
Investigate per-receiver light loops, coverage fallbacks, visibility and highlight
response before redesigning styled storage or further small shadow optimisations.
Snapshots show 16 selected lights plus a 120-light baked fallback tail; establish
which pixels actually use that tail rather than interpreting global counts as
per-pixel work. Preserve finite-emitter and mixed-shadow correctness.
All temporary diagnostic source switches were removed, exact originals restored
and the normal runtime rebuilt. Playable install/updater remain unchanged.
