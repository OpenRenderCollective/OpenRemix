# Forward+ delivery roadmap

The intended result is a cheaper, Source-2-style rendering backend for GMod: forward PBR shading with
MSAA, hardware map-load surface lightmaps and probes, attributed baked direct lighting, soft static
shadows combined with dynamic-object shadows, automatic tolerant parallax cubemaps, rough SSR, and
optional hardware reflections/GI. Replacement materials must be shared with path tracing. Water must
remain compatible with the same authored optical materials.

This roadmap tracks delivery requirements. Detailed measurements and run evidence belong in local
reports. An implementation or synthetic test alone does not close an integration or visual-quality gate.

## Current capabilities and open requirements

| Requirement | Current state | Remaining proof or work |
| --- | --- | --- |
| Modular forward PBR, clustered lights and 1x/2x/4x MSAA | Implemented | Refresh current-build engine coverage through the integration milestone below |
| World-metric surface charts, atlas budgets and irradiance probes | Implemented; warped boundary fans, modulating decals and lit displacement paint layers supported | Inspect connected-face continuity, seam donors, close decal receivers, indirect noise and budget coarsening |
| Dynamic receiver agreement with surface lightmaps | Budget-fitted grid, bounded recovery of BSP-solid slots, shared-position receiver diagnostics, static sun visibility and sparse local-light visibility; experimental aggregate direct grid lighting and moving corrections tested | Close room-scale/deferred-light coverage and strict attributed reconstruction gaps; validate moving actors across maps using CS2, Half-Life: Alyx and s&box as references |
| Soft finite emitters, attributed direct diffuse and live specular | Implemented | Compare visible penumbrae and light separation in the live engine |
| Receiver-area antialiasing of baked direct shadows | Adaptive sunlight edge refinement integrated; grouped-site local transport available | Integrate local/overflow refinement, improve subpixel coverage detection and inspect more maps/views |
| Aggregate fully static diffuse and compact directional lightmaps | Controller integration retains attributed masks/SH beside aggregate RGB; surface/probe publication, moving shadows and cache reload tested at 1x/2x/4x MSAA; definition edits/removal invalidate stale data until recapture; experimental fully baked policy skips per-light response on covered receivers and retains analytic fallback; optional adapter bake hints preserve mixed requirements and identify sphere-shaped surface emission; covered receivers cluster only the mixed/dynamic prefix, with the complete light tail retained for uncovered fallback; default remains legacy | All-mixed aggregate alone did not improve steady-state cost; fully baked policy changes specular/moving-shadow semantics and now prioritizes mixed-shadow slots with timed fades and retains spare fallback coverage; still needs broader live receiver coverage and water/probe/RT parity and broader-map quality before rollout |
| Dynamic-object shadows combined with baked static shadows | Implemented; captured static casters excluded; overflow audits classify constant visibility and retain bounded-error palette masks within a memory budget | Confirm retained coverage across maps and bake quality settings; budget-exhausted overflow still requires realtime shadow capacity/range; live receivers, characters and static membership after map changes |
| Indexed visibility storage beyond four channels | Shared R8 pages and compressed scalar masks preserve chart/light identities; publication chooses pages only when actual image allocations shrink, planning candidates before decoding only the winner | Storage does not reduce visibility ray counts; finish automatic mixed selection and aggregate diffuse integration to address dense-map bake time |
| Automatic parallax cubemap bounds with tolerance for room details | Coverage-based selection and optional 32-capture capacity; receiver bounds contact nearby axis-aligned room surfaces independently of inset projection boxes | Limited budgets still leave rooms uncovered; review transitions, rotated rooms and reflection leaks across multiple maps |
| Rough SSR, optional hardware reflections and live GI | Implemented | Refresh actual-engine toggles and shared-scene lifetime coverage; continue measured performance work where needed |
| Shared USD/PBR material replacements | Implemented; plain Source roughness fallback corrected | Refresh late material arrival and broader material/BRDF parity in the current engine build |
| Water optics, normals, HDR refraction, SSR/RT and underwater composition | Implemented; enabled for new settings; lazy refraction snapshots | Broader underwater/lifecycle validation, noise and live visual comparison with path-traced material response |
| OIDN indirect-lightmap filtering | Implemented; defaults on where available, raw fallback without OIDN | Connected-seam review and lower-sample quality comparisons; distribution of optional dependencies |
| Usable build and deployment | Feature branches/remotes retained; Windows runtime bundler available | Remaining platform/dependency coverage, lifecycle smoke tests and final delivery instructions |

Forward+ displays a passive map-lighting indicator while preparing or baking, even
when settings are closed. It shows the current stage's work counts when available
and a spinner through stages without a known total, including finalization. Probe
completion is not treated as overall bake completion. The indicator disappears
when baked lighting is active; a failed bake instead displays its failure message.

## Next milestones, in order

1. **Replace exhaustive overflow baking.** Complete indexed static local-light visibility for probe
   receivers; static sun visibility already uses the shared probe interpolation, with moving-caster
   shadows and complete shadow maps retained for uncovered receivers. Aggregate ordinary fully static
   direct diffuse with indirect lighting; retain separate light identity only where mixed lighting needs it. Select mixed lights
   automatically from legacy inputs, preserving important emitters and reusing mask channels where
   receiver coverage permits. Define a correct fallback for overlapping mixed lights instead of
   silently losing their shadows. Keep camera-dependent specular visibility and dynamic-object
   shadow behavior explicit: fully baked diffuse cannot supply either by itself. Evaluate compact HDR
   irradiance plus lower-resolution directionality against the current SH representation before
   replacing it. Require equal-energy, no-double-counting, wall-occlusion and moving-caster tests,
   followed by measured fresh-map bake time, GPU memory and close normal-mapped surface comparisons.
   Geometry-emitter hints alone do not address maps dominated by explicit point/spot lights. Extend
   aggregate ownership to those fixed emitters while retaining compact, selected per-light specular
   visibility and moving-caster corrections; avoid restoring exhaustive evaluation for every baked
   emitter. Validate this separately on maps with no texture-emission lights, including Steamlab.
   The opt-in `experimentalSelectedBakedLights` runtime policy now keeps aggregate corrections and
   direct highlights only for lights holding a mixed-shadow slot (including outgoing fades).
   Unselected fixed lights retain aggregate diffuse and environment reflections; uncovered receivers
   retain the full analytic fallback. Styled/moving lights and adapter-required mixed lights are
   exempt. This reuses attributed bake data and does not reduce its preparation cost. Broader quality,
   independent specular selection and cheaper on-demand correction visibility remain open; leave the
   option off by default until those tradeoffs are accepted in live maps.
   F12 exposes a single default-off **Baked lighting preview (experimental)** control. It derives
   aggregate diffuse, baked geometry emitters and selected mixed responses without overwriting
   the underlying profile preferences. Map baking and baked direct diffuse must be enabled;
   supported live GI temporarily pauses the preview. Disabling it restores the requested policy.
   The UI explicitly describes omitted direct highlights and moving-object shadows. This is a
   comparison tool for the unfinished baked-light policy, not the final quality default.
   The preview independently selects up to 16 attributed fixed-light highlights, with timed fades
   and captured-emitter keys rather than potentially shared adapter light IDs. A separate GPU
   highlight weight preserves mixed-shadow strength when the selections differ. Outgoing highlights
   retain their bounded assignments until their weight reaches zero. The experimental profile keys
   `experimentalBakedHighlightLights` (0..32) and `experimentalBakedHighlightFade` (0..10 seconds)
   control that budget and fade independently of shadow maps. Pure baked geometry emitters still
   omit analytic highlights. Camera-distance/influence ranking is an initial approximation; test
   live-map highlights, leakage and frame cost before changing the default lighting policy.
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
   version capturing host copies regressed and was rejected. Continue device/workload comparisons
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

## Work discipline

Each checkpoint records its changed files/commit, evidence, limits, and the next action. A failed
experiment remains a failed experiment. A running process is tracked by its actual handle until it
finishes. Performance improvements include before/after configuration and output comparisons.
Milestones close against their stated gates; newly useful features do not replace unfinished gates.
