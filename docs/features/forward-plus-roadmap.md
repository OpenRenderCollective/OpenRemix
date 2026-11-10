# Forward+ quality and performance roadmap

This is the ordered engineering queue, not a claim that each reported defect has
been reproduced. The Source 2 preset is our configuration name, not implementation
parity. Read the [handoff](forward-plus-handoff.md) and
[architecture](../architecture/forward-lighting.md).

## Decision rules

Finish one measurable improvement per PR. Distinguish reports, hypotheses,
reproductions and verified causes. Record a failing pose and effective settings
before modifying an algorithm. Use existing diagnostics first. After two failed
implementation hypotheses, stop coding and summarize the evidence for review.
Do not expand into a new renderer or GI provider to escape an unresolved diagnosis.

## 1. Cubemap correctness

Reported cases: d1_trainstation_03, room with two people at the windows and a TV
(missing reflections); d1_trainstation_02, courtyard-to-apartment entrance
(floor/out-of-bounds reflection); gm_construct, flat grassy area (coverage gap and
apparent sky-only capture). Record exact poses and BSP identities. An image alone
does not prove the capture origin is invalid.

Diagnosis order:
1. Verify content, bake completion, cache identity and authored overrides, including
   an explicitly empty set. Distinguish disabled capture from stale publication.
2. Display origin, influence volume, projection volume, selected IDs, blend weights
   and capture layer. These are distinct; a volume center need not be a valid origin.
3. Inspect raw cube faces. Bad faces implicate capture geometry, origin validity or
   publication; correct faces direct investigation toward selection/projection.
4. Compare unculled and optimized candidate selection. If a valid candidate survives
   but contributes darkness, inspect normalization/material response instead.
5. Inspect oriented ray-box projection and blending. Do not inflate every box to
   conceal coverage defects or override authored intent silently.

Keep origins in valid connected space and handle unavailable valid sites explicitly.
Share placement hints with diffuse probes only where useful; their data and density
need not be identical.

Done: the three repros and moving boundary sweeps have appropriate coverage without
new sky contamination, with cold/warm restore, oriented boxes, overlap priorities,
water and viewmodels checked where affected. Separate distinct causes into PRs.

## 2. Lightmap leakage, then UV utilisation

Distinguish transport visibility, invalid sample positions, interpolation across
geometry, denoiser bleed and atlas/mip bleed. Compare raw bake samples, nearest
sampling, ordinary filtering and denoiser-off diagnostics. Disabling a component
is diagnostic, not the shipping fix.

For Trainstation wall/floor and storefront intersections, show chart IDs, UVs,
texel footprints, sample positions and visibility. Split at an intersection only
when reconstruction crosses the occlusion boundary. Splitting does not repair an
incorrect visibility ray or a sample inside a wall. Consider discontinuity-aware
sampling if topology splitting is too costly. Include thin walls, T-junctions,
grazing views and styled layers.

Measure occupied texels, padding, chart count and world-space density before packing
changes. More charts can reduce bleed and increase padding waste. Preserve density
and mip-safe gutters; report allocation/file bytes, not just packing percentage.

Done: the seam stops leaking without artificial darkening, including grazing views
and warm restore. Atlas packing should be a separate follow-up unless required.

## 3. Diffuse probe quality

Move a fixed-material prop through a doorway and along a wall/floor. Inspect chosen
positions, validity, weights and directional lighting. Separate probes inside solids,
interpolation across rooms, insufficient spatial density and insufficient directional
representation. Use constant-normal/material diagnostics to separate lighting from
specular and tone-mapping artifacts.

Visibility-aware weights must normalize surviving contributors and handle all-invalid
stencils without black holes. Do not add per-pixel rays by default. Increase density
near floors/boundaries only when evidence identifies a sampling problem; shared
playable-space hints do not require unified reflection/diffuse volumes.

Done: continuous motion retains gradients without leakage or popping; styled/mixed
lighting remains correct. Report probe count, runtime cost and cache growth.
A wholesale DDGI replacement is out of scope.

## 4. Reflection roughness agreement

Use a small dielectric/metal roughness ramp at normal/grazing angles and several hit
distances. Match lighting, material inputs and exposure; compare linear HDR and final
output. Trace roughness through material decoding, specular antialiasing, packed-guide
MSAA selection, SSR mip choice, BRDF weighting and fallback composition.

Check perceptual roughness versus microfacet-alpha at every boundary. Do not square
roughness again merely because a reference does so elsewhere. SSR currently uses a
screen-colour mip footprint approximation, not full rough-lobe integration. Isolate
SSR from cubemap fallback; compare lobe width separately from energy/normalization.
Inspect existing guide contracts before adding a heavy prepass or more attachments.

Done: explain and reduce measured mismatch across the ramp, distances and fallback
transitions. Record residual approximation and cost; do not promise exact PT parity.

## 5. Map-size-dependent GPU work

Use completed stable bakes in matched c1a2d/hls03amrl hallway views. Similar rooms
invite similar local shading work, but geometry, light overlap, shadows, materials
and cache behavior can differ. Equal FPS is not a correctness invariant.

Measure candidate generation, cluster construction, actual fragment light visits,
shadow work, reflection lookup and residual opaque shading separately. Static diffuse
baking does not eliminate optional analytic specular or moving-caster corrections.
Audit energy ownership before deleting loops; avoid duplicate baked/runtime energy.

If cluster work scales with irrelevant map lights, trial conservative spatial/view
candidate generation upstream. Preserve ordering, styles, mixed lights, moving
receivers, offscreen casters and exact overflow. Unknown/unbounded lights need a
conservative path. Never replace this with an arbitrary four-light cap or range cutoff.

Done: repeated total-GPU improvement with image parity and no material p95 regression
at 1080p/4K, normal gameplay receivers and gmod_mcore_test 1. Report median/p95, GPU
passes and CPU time. Do not sum nested timers or interpret diagnostic omission deltas
as additive achievable savings. A power-limited 4090 does not emulate another GPU.
Other-hardware qualification can come from contributors; do not claim it locally.

## 6. Bake size and lifecycle

Inventory bytes by surfaces, style layers, probes, cubemaps, padding and metadata.
Separate disk size, peak bake memory, GPU residency and load time. Eliminate redundant
payloads before choosing a codec. Evaluate lossless per-payload compression where
appropriate; disk compression need not reduce GPU memory and may increase load peaks.

Lossy formats require HDR/style-gradient/rough-reflection error checks. Schema changes
need versioned invalidation and corrupt/partial-file rejection. Reuse compatible bakes
and the shared playable cache, not duplicate benchmark caches.

Done: smaller representative bakes with measured quality, load time and memory effects;
atomic publication and warm restore continue working.

## Work sequence and boundaries

Reproduce -> discriminate causes -> one correction -> relevant tests -> live check ->
review diff -> focused PR. A test should exercise the failure, not mirror the code.
Stop when the bounded acceptance criteria pass; do not keep adding adjacent ideas.
Defer alternative GI, language migrations, editor rewrites and broad material redesign.
Existing decals, styles, fog, water, viewmodels and transitions remain regression
obligations where touched.

References support specific mechanisms. Record revision/function and input conventions.
ValveResourceFormat is a Source 2 reference/reconstruction, not native CS2 source.
A preset name or FPS screenshot does not establish algorithm parity.
