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

- `forward/bake/adaptive_grid.*`: coarse placement and receiver/reference-driven refinement selection.
- `receiver_support.*`: bounded receiver support, halos and deterministic neighbour collection.
- `refinement_builder.*`: coverage-first regional subdivision, canonical shared physical records and compaction.
- `grid_directory.cpp`: directory validation, ownership and reciprocal compatible-face flags.
- `grid_sampling.cpp`: CPU reference reconstruction and coarse/fine transition weights.
- `controller_refinement.cpp`: asynchronous refinement scheduling between coarse/required surface and fine work.
- `shaders/grid-stencil.glsl`: matching shared stencil for diffuse, sky, sun and local visibility/corrections.
- Existing cache/controller/publication modules retain lifetime, cancellation, fencing and validated import.

The prototype cache revision is 24; stable deployed revision is 23. GPU publication packs the mode and
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

The adaptive candidate improves five of six rendered receiver comparisons, with one worse. Controlled
replay previously measured roughly 40% higher GPU time versus the installed uniform renderer. A packed
directory-offset optimization reduced cost within the candidate by about 6.5%; this does not erase the
regression. The hall's visible coverage is overwhelmingly charted, and the six test receivers are covered;
large missing visible coverage did not explain this camera's cost. Fragment counters confirm that the
existing depth prepass substantially reduces opaque colour shading work. The unmapped-receiver
specialization reduces compiled shader size and modestly improves the tested replay, but the larger
adaptive regression remains unresolved. Continue with probe/stencil cost and partially mapped receivers.
Keep GPU frame, transport and startup timing separate.

Rejected and removed: generic fragment offline optimization, fully fine-weighted coarse-sampling shortcut,
separate mapped fragment modules, reordered category profiling, homogeneous draw-range splitting,
compact cell-address stencil indices, unified mapped/partial pipelines, and explicit products replacing
fixed-power stencil weights. These preserved output in
many controls but did not establish a performance gain; reordered profiling changed coplanar pixels.
Some paired runs drifted despite stable clocks: do not treat small percentages as precise hardware laws.
Driver local-memory statistics were implausible; do not silently mask them into invented spill counts.

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
