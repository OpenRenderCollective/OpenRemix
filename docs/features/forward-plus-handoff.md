# Forward+ continuation guide

## Start here

This feature branch contains an unreleased adaptive irradiance candidate, not an accepted faster
replacement for the deployed uniform-grid renderer. Read the current-status block and requirement
matrix in [the roadmap](forward-plus-roadmap.md), then [the renderer guide](forward-plus.md).
The last deployed native checkpoint is `e51bff0`; adapter checkpoint `712a3aa` is the pre-handoff base.
Current checkpoint commits preserve subsequent work for review. Main branches remain upstream.

First decide whether to finish, explicitly isolate, or defer adaptive refinement. The candidate currently
runs automatically when eligible; it must not silently become the default shipping performance policy.
Keep static visibility, unknown coverage, toggleable groups and offscreen moving shadows correct.

## Module ownership

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

The current probe-planning cache policy revision is 26; the handoff prototype used 24 and the deployed
uniform build uses 23. Grouped probe policy version 2 also separates caches with incomplete controllable
groups. Directional probe depth remains opt-in and is not deployed. Upgrading deliberately rebakes once;
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
`gm_fork`, `d1_trainstation_02` and installed large combined Half-Life addon maps. These additions
are pending coverage. Inventory addon BSP names and verify actual loaded content before recording
cold/warm, fixed-view and armed-traversal results; do not substitute repeated small-map checks.
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
these two volumes do not establish whole-map reflection coverage.

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
motion/lighting guides still need acceptance. This is not deployed
and Trainstation is still blocked. Define stable mean optical tint explicitly because tint can share
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
