# Remix native runtime

This repository is an SDL3/Vulkan renderer with the retained Remix 0.1003 API.
Raster remains the default; an opt-in ray-query backend adds diffuse world lighting.
Gameplay effects and UI remain rasterized. Compatibility exports remain supported;
the original RTX renderer and Direct3D injection runtime have been removed.

Use Clang, Meson and Ninja; on Windows use clang-cl in a Visual Studio developer environment.
First-party code is grouped under `src/native/{api,runtime,scene,raster,pathtrace,platform,ui}`.
Tests mirror feature folders under `tests`. See [architecture](docs/architecture.md) for ownership.
CPU-only `scene` code must not depend on Vulkan, SDL, or either GPU backend. Backend GPU state
is private to each feature. Preserve existing API locking, resource lifetimes, callback re-entry,
and GPU synchronization when splitting or modifying methods.

Keep `public/include/remix` layouts, calling conventions and exports compatible.
Keep vendor sources/pins in `third_party` and the `include/vulkan` submodule unchanged unless
updating that dependency explicitly. Use two-space indentation and attached braces;
`.clang-format` covers maintained first-party C/C++ only.

Build: `CC=clang CXX=clang++ meson setup _work/build/native && meson compile -C _work/build/native`.
Test: `meson test -C _work/build/native --print-errorlogs`.
GPU tests require `-Dgpu_tests=true`, an SDL video driver and Vulkan device.
Enable `REMIX_VULKAN_VALIDATION=1 VK_LAYER_VALIDATE_SYNC=1` with the validation layer installed.
Shared build/package tools are in `scripts/common`; platform entry points are in `scripts/<platform>`.
Keep local outputs under `_work/{build,deps,packages,reports}`. Meson manages its SDL fallback
source/cache under `subprojects`; `SDL3.wrap` is tracked.

Readable shaders and generated SPIR-V are co-located in each renderer's `shaders` folder.
Normal builds need no shader compiler. Use `python3 scripts/shaders/generate.py all --check`
and `--validate` to check source/binary agreement and SPIR-V validity.
Path-tracing SPIR-V is optimized with `spirv-opt -O` (inlined modules compile about twice as
fast on NVIDIA) and stored as XZ streams in `pathtrace/shaders/embedded.bin`; `embedded.h`
indexes them and `trace::shader()` decompresses a stage on first use (vendored XZ Embedded).
Regeneration needs glslang and SPIRV-Tools and compiles every stage concurrently.
Standalone driver probes in `tools/vulkan-probes` require `-Dvulkan_probes=true` and shader tools.
Experimental MoltenVK tooling in `tools/moltenvk` remains separate from SDK-based platform builds.
`-Dgpu_backend=metal` builds the native Metal backend in `src/native/metal` instead of the Vulkan
runtime (macOS; see `docs/features/metal-backend.md`). Its libraries are generated from the same
shaders by `scripts/shaders/generate.py metal`; check them with `generate.py metal --check`.

Preserve `ref/`, Git history, linked worktrees, and archived evidence. Do not discard ignored
artifacts without an explicit cleanup scope from the user. Track architecture and feature docs only;
`.gitignore` allows the existing guides plus `docs/architecture/` and `docs/features/`.
Add new standalone architecture/feature guides to that allowlist. Required license notices stay tracked.
Write testing, verification, benchmark, and deployment reports under `_work/reports/`, not tracked docs.
Legacy reports in `docs/` remain local and ignored. Do not force-add reports or link tracked guides
to local testing evidence. Historical report paths describe the recorded revisions.
