# Forward+ controls

This catalog separates saved renderer requests, production-path ablations,
private experiments and diagnostics. A `REMIX_DEBUG_*` name alone does not mean
that its implementation is unused or disabled by default.

The scope is literal environment names read by native Forward+ and its captured
scene input diagnostics. Shared path-tracing, upscaling, platform controls and
adapter-specific controls are outside this catalog. PR #37 adds its own cascade
settings on the unmerged branch; those are not main-branch settings.

## Saved requests

[settings.cpp](../../src/native/runtime/settings.cpp) registers the canonical
`rtx.native.forward.*` keys and their validators. [Options](../../src/native/forward/options.h)
holds requests; [techniques](../../src/native/forward/techniques.h),
[lighting ownership](../../src/native/forward/lighting_plan.h) and the
[resource plan](../../src/native/forward/provider_plan.h) resolve their supported
combination. Quality/display/cache preferences remain separate from technique
selection. A saved value need not be active under every selected technique.

Five canonical keys retain historical experimental names:
`experimentalAggregateDiffuse`, `experimentalFullyBakedLights`,
`experimentalBakedGeometryLights`, `experimentalGroupedStyledDirect` and
`experimentalSelectedBakedLights`. They are low-level policy requests, not aliases
for the same field. The resolved baked-receiver plan deliberately derives
aggregate, geometry, grouped-style and mixed-light ownership. Preserve the saved
requests for other supported combinations instead of deleting or renaming them.
`debugView` is a diagnostic view selector; the remaining canonical keys request
techniques, quality, display or scheduling policy subject to their validators.

The displayed Source 2/Custom preset is derived from these requests. Do not add
another persisted preset selector that can disagree with them.

## Environment evaluation

Set developer overrides before launch. Some readers cache their first result;
others evaluate at target creation or while recording/preparing work. There is
no common live-reload contract for environment variables.

In the tables, **presence** means that even an empty value or `0` activates the
override. **`1`** means exact string equality. **Except `0`** means unset/default
is enabled and only the exact string `0` disables it. These different parsers
are existing behavior, not interchangeable boolean settings.

## Storage locations

| Name | Role |
| --- | --- |
| `REMIX_CONFIG_DIR` | Shared configuration/profile location; also supplies the default Forward+ cache root. |
| `REMIX_BAKE_CACHE_DIR` | Explicit Forward+ cache directory override. |

See [cache_directory.h](../../src/native/forward/cache_directory.h) for fallback
paths. These are supported location overrides, not experiments.

## Production paths with developer overrides

The underlying paths are used by the normal renderer. Their private switches
remain useful for ablation and regression checks; they are not preset choices.

| Name | Override behavior | Owner |
| --- | --- | --- |
| `REMIX_DEBUG_BAKE_MODEL_CHARTS` | Unset follows `lightmapModels`; `1` forces on, other present values force off. Eligibility constraints still apply. | [storage_options.h](../../src/native/forward/bake/storage_options.h) |
| `REMIX_DEBUG_DIRECTIONAL_FIELDS` | Enabled except `0`; publication and shader specialization must agree. | [storage_options.h](../../src/native/forward/bake/storage_options.h) |
| `REMIX_DEBUG_DIRECT_FETCH` | Enabled except `0`; selects which aggregate direct owners need unit-direct fetches. | [scene.cpp](../../src/native/forward/scene.cpp) |
| `REMIX_DEBUG_UNIFIED_MASKS` | Enabled except `0`; derives unified visibility storage when it fits. | [controller_storage.cpp](../../src/native/forward/bake/controller_storage.cpp) |
| `REMIX_DEBUG_MASK_PAGES` | Enabled except `0`; permits indexed visibility-page preparation. | [diffuse_publication.cpp](../../src/native/forward/bake/diffuse_publication.cpp) |
| `REMIX_DEBUG_DISABLE_PROBE_BATCH` | Presence disables the default attributed-probe batch cache. | [renderer.cpp](../../src/native/forward/renderer.cpp) |
| `REMIX_DEBUG_DISABLE_VIEWMODEL_EARLY_DEPTH` | Presence disables the normal early-depth viewmodel variant. | [pipelines.cpp](../../src/native/forward/pipelines.cpp) |
| `REMIX_DEBUG_FORWARD_UNCLUSTERED_FALLBACK` | Presence disables clustered baked-tail fallback, retaining the linear fallback contract. | [scene.cpp](../../src/native/forward/scene.cpp) |
| `REMIX_DEBUG_FORWARD_VIEWMODEL_UNCULLED_PROBES` | Presence replaces the viewmodel reflection candidate mask with all candidates. | [viewmodel/scene.cpp](../../src/native/forward/viewmodel/scene.cpp) |

Legacy atlas/visibility and linear light-list fallbacks also serve unsupported
allocation/coverage cases. An ablation flag being rarely used does not establish
that its fallback implementation can be removed.

## Private experiments awaiting wider acceptance

None of these is promoted to a new Source 2 default by its presence in the tree.

| Name | Activation | Role / outstanding acceptance |
| --- | --- | --- |
| `REMIX_DEBUG_COARSE_TRANSFER_PREPARE` | `1` | Prepare the coarse transfer prototype without selecting it for sampling. |
| `REMIX_DEBUG_COARSE_TRANSFER_SAMPLE` | `1` | Prepare and sample that prototype; wider quality/runtime acceptance remains open. |
| `REMIX_DEBUG_DIRECTIONAL_PROBE_DEPTH` | `1` | Directional probe-depth reconstruction; broader map quality and cost remain open. |
| `REMIX_DEBUG_PROBE_LOCAL_CAPTURE` | `1` | Derive/restore optional receiver-local reflection captures and evidence. |
| `REMIX_DEBUG_PROBE_LOCAL_SELECTION` | `1` | Select prepared local replacements when matching evidence is available; default remains the ordinary capture source. |
| `REMIX_DEBUG_PROBE_REGIONAL_COVERAGE` | `1` | Enables the bounded regional-coverage derivation used by capture diagnostics, not a shipping quality preset. |

Owners: [storage_options.h](../../src/native/forward/bake/storage_options.h),
[local capture scheduling](../../src/native/forward/bake/controller_local_reflections.cpp),
[local selection](../../src/native/forward/renderer_local_reflections.cpp),
[capture diagnostics](../../src/native/forward/probes/capture_dump.cpp) and
[pipeline selection](../../src/native/forward/pipelines.cpp).

## Reference and qualification modes

These change the work being measured, not just the amount of logging.

| Name | Activation / role |
| --- | --- |
| `REMIX_DEBUG_BAKE_CPU_VISIBILITY` | `1` selects the CPU visibility reference route. |
| `REMIX_DEBUG_BAKE_REFERENCE_POINTS` | Input file selecting bounded point/geometry/radiance reference work; it is not an ordinary full-map bake. |
| `REMIX_DEBUG_BAKE_SUN_DEPTH` | Presence requests auxiliary sun-depth capture. |
| `REMIX_DEBUG_BAKE_SURFACE_SAMPLES` | `1` retains raw surface samples and bypasses the ordinary cached shortcut for that qualification. |

Read [bake preparation](../../src/native/forward/bake/controller.cpp),
[reference capture](../../src/native/forward/bake/reference_capture.cpp) and
[pipeline selection](../../src/native/forward/pipelines.cpp) before comparing such
a run with ordinary rendering.

## Diagnostic output and profiling

Output paths/prefixes are nonempty strings unless stated otherwise. Diagnostics
can add GPU queries, bake rays, retained samples or large binary dumps. They are
not optimization presets and should be identified in performance evidence.

| Name | Output or measurement |
| --- | --- |
| `REMIX_DEBUG_BAKE_CAPTURE_PREFIX` | Capture/publication binary dumps and requested frame readback prefix. |
| `REMIX_DEBUG_BAKE_GRID_PREFIX` | Narrower probe/grid publication dumps without the full surface capture. |
| `REMIX_DEBUG_BAKE_INPUT_PREFIX` | Captured scene input fingerprints. |
| `REMIX_DEBUG_BAKE_MODEL_ATLAS_PREFLIGHT` | Atlas report path; deliberately stops before scheduling the normal bake. |
| `REMIX_DEBUG_BAKE_MODEL_PREFLIGHT` | Model eligibility report path; deliberately stops before scheduling the normal bake. |
| `REMIX_DEBUG_BAKE_MODEL_CHART_MESH` | Prefix for bounded generated model-chart mesh exports. |
| `REMIX_DEBUG_BAKE_MODEL_RECEIVERS` | Model receiver report path. |
| `REMIX_DEBUG_BAKE_PHASE_HASHES` | Phase/input fingerprint output path. |
| `REMIX_DEBUG_BAKE_RECEIVERS` | Receiver report prefix, including optional grouped-direct diagnostics. |
| `REMIX_DEBUG_BAKE_TIMING` | Presence enables additional bake-stage timing/progress logging. |
| `REMIX_DEBUG_BAKE_VISIBILITY_REPORT` | Static visibility report path. |
| `REMIX_DEBUG_COARSE_TRANSFER_CPU_REPORTS` | `1` requests prototype CPU comparison reports and implies transfer preparation. |
| `REMIX_DEBUG_COARSE_TRANSFER_OUTPUT` | Prepared coarse transfer binary output path. |
| `REMIX_DEBUG_DIRECT_OWNERSHIP` | Direct-light ownership report path. |
| `REMIX_DEBUG_FORWARD_DRAW_COUNTS` | Presence enables draw/receiver count diagnostics. |
| `REMIX_DEBUG_FORWARD_FRAGMENT_STATS` | CSV output path; allocates fragment/occlusion query instrumentation when supported. |
| `REMIX_DEBUG_FORWARD_RECEIVER_TIMINGS` | Presence adds receiver-class GPU scopes when timing is active. |
| `REMIX_DEBUG_FORWARD_REFLECTION_TIMINGS` | Presence adds reflection-stage GPU scopes when timing is active. |
| `REMIX_DEBUG_FULL_REFLECTION_INPUTS` | `1` expands requested reflection frame dumps; requires the capture/readback setup. |
| `REMIX_DEBUG_MASK_COSTS` | Presence enables mask cost auditing and bypasses the initial ordinary budget rejection. |
| `REMIX_DEBUG_PROBE_CANDIDATES` | Fitted capture candidate report path. |
| `REMIX_DEBUG_PROBE_GUARD_VISIBILITY` | Guard-visibility report path; can schedule additional diagnostic work. |
| `REMIX_DEBUG_PROBE_LOCAL_IMAGE` | Prepared local reflection image binary output path. |
| `REMIX_DEBUG_PROBE_RECEIVER_RAYS` | Small receiver-query input file; writes a results companion. |
| `REMIX_DEBUG_PROBE_RECEIVER_VISIBILITY` | Receiver/capture visibility report prefix and supporting audit work. |
| `REMIX_DEBUG_REFLECTION_BATCH_TIMINGS` | `1` enables ray-reflection batch profiling. |

Readers live beside their [bake](../../src/native/forward/bake),
[capture](../../src/native/forward/probes), [mixed-light](../../src/native/forward/mixed),
[image dump](../../src/native/forward/image_dump.cpp) and
[fragment statistics](../../src/native/forward/fragment_statistics.cpp) owners.

## Retirement decisions

The internal `Options::bakeConfiguration()` forwarding helper was removed.
Frame setup also consumes the existing resolved `lightingPlan`, so the obsolete
`bakedLightingActive()` and `selectBakedLights()` helper methods were removed with
their callers. Baking and frame receiver decisions use one plan; tests inspect
its fields directly. The ownership resolver, saved controls and behavior remain
unchanged.

No lighting technique or fallback is declared retired by this catalog. The
reviewed default-on paths, regression modes and pending experiments have live
readers. Removing a control requires checking its tests, saved-key compatibility, cache/publication
semantics and fallback callers together. Dynamic/concatenated controls and the
remaining native/adapter controls still require separate review. This catalog
does not complete the broader performance or quality acceptance queue.
