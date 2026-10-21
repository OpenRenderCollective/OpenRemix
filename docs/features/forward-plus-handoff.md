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

- `forward/shadows/sun_cache.*`: frozen sunlight ownership and projection reuse. Subtexel fit
  changes retain the exact cached matrix for both static sampling and moving-caster rendering;
  significant movement, scene replacement and resolution changes still invalidate. This does not
  remove uncovered-receiver fallback cascades. Broad live shadow-quality acceptance remains open.

- `forward/bake/adaptive_grid.*`: coarse placement and receiver/reference-driven refinement selection.
- `receiver_support.*`: bounded receiver support, halos and deterministic neighbour collection.
- `refinement_builder.*`: coverage-first regional subdivision, canonical shared physical records and compaction.
- `grid_directory.cpp`: directory validation, ownership and reciprocal compatible-face flags.
- `grid_sampling.cpp`: CPU reference reconstruction and coarse/fine transition weights.
- `controller_refinement.cpp`: asynchronous refinement scheduling between coarse/required surface and fine work.
- `adaptive_grid.cpp` / `sparse_grid.cpp`: an empty coarse seed retries ordinary full-budget placement;
  an empty density-search trial keeps the last verified field. `EmptyProbePlacement` is distinct from
  cancellation or malformed inputs. A wholly unusable partition still rejects.
- `shaders/grid-stencil.glsl`: matching shared stencil for diffuse, sky, sun and local visibility/corrections.
- `probe_visibility_plan.cpp`: conservative per-record light volumes; adaptive records keep their largest
  referencing/trace support, while shared coarse corners retain coarse reach.
- Existing cache/controller/publication modules retain lifetime, cancellation, fencing and validated import.

The current probe-planning cache policy revision is 25; the handoff prototype used 24 and the deployed
uniform build uses 23. Upgrading deliberately rebakes once; camera movement and runtime toggle values
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
found to contain an empty Construct sidecar; it is preserved pending an explicit deployment decision.

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
Native lighting remains engine agnostic. `bsp-replay` supports repeatable default-off
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
