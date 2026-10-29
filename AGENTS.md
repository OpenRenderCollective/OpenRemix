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
updating that dependency explicitly. NUMOS (sky) is the git submodule `third_party/numos/numos-sky`,
pinned in `third_party/numos/upstream.json`; check it out without `--recursive`
(`git submodule update --init third_party/numos/numos-sky`). `-Dsky=auto` builds without it.
FSR on Windows loads AMD's signed ffx-api DLLs from the FidelityFX SDK 2.4.0 submodule
`third_party/fidelityfx/FidelityFX-SDK`, pinned in `third_party/fidelityfx/upstream.json` and
required by Windows builds (`git submodule update --init third_party/fidelityfx/FidelityFX-SDK`);
Linux and macOS link FSR 3.1.4 from `third_party/fsr31` (`upscale/fsr.h`).
XeSS Super Resolution is Windows only, like FSR 4: it loads Intel's signed `libxess.dll` from the
XeSS SDK 3.0.2 submodule `third_party/xess/xess`, pinned in `third_party/xess/upstream.json` and
required by Windows builds (`git submodule update --init third_party/xess/xess`; `upscale/xess.h`).
Use two-space indentation and attached braces;
`.clang-format` covers maintained first-party C/C++ only.

Build: `CC=clang CXX=clang++ meson setup _work/build/native && meson compile -C _work/build/native`.
Test: `meson test -C _work/build/native --print-errorlogs`.
GPU tests require `-Dgpu_tests=true`, an SDL video driver and Vulkan device.
Enable `REMIX_VULKAN_VALIDATION=1 VK_LAYER_VALIDATE_SYNC=1` with the validation layer installed.
Shared build/package tools are in `scripts/common`; platform entry points are in `scripts/<platform>`.
Keep local outputs under `_work/{build,deps,packages,reports}`. Meson manages its SDL fallback
source/cache under `subprojects`; `SDL3.wrap` is tracked.

Readable shaders live in each renderer's `shaders` folder. The build generates their SPIR-V
(and the Metal libraries) into the build directory with `scripts/shaders/generate.py`; never commit
generated `embedded.h`, `embedded.bin` or `embedded_data.cpp`. The tools are glslang and
SPIRV-Tools (and for Metal the SPIRV-Cross fork) at the revisions in `toolchain.lock.json`, which
`scripts/shaders/toolchain.py` builds once per machine into `_work/tools/shader-toolchain`
(`-Dshader_tools=` moves the cache). With NUMOS, it also fetches the pinned slangc release;
`scripts/shaders/sky.py` compiles the NUMOS passes and `src/native/sky/shaders/*.comp.slang` and
checks them against NUMOS's pass table and struct layouts (not its SPIR-V hashes, which come from
a Remix-only slangc build). Only NRD's DXC SPIR-V (`denoise/shaders/nrd_embedded.h`),
FSR 3.1's (`upscale/shaders/fsr31.h`) and the blue-noise tables stay committed; check them with
`generate.py nrd --check` / `fsr31 --check`.
Path-tracing SPIR-V is optimized with `spirv-opt -O` (inlined modules compile about twice as
fast on NVIDIA) and stored as XZ streams in `pathtrace/shaders/embedded.bin`; `embedded.h`
indexes them, `embedded_data.cpp` embeds the blob and `trace::shader()` decompresses a stage on
first use (vendored XZ Embedded).
Standalone driver probes in `tools/vulkan-probes` require `-Dvulkan_probes=true` and shader tools.
Experimental MoltenVK tooling in `tools/moltenvk` remains separate from SDK-based platform builds.
`-Dgpu_backend=metal` builds the native Metal backend in `src/native/metal` instead of the Vulkan
runtime (macOS; see `docs/features/metal-backend.md`). The build generates its libraries from the
same shaders (`scripts/shaders/generate.py metal`).

Preserve `ref/`, Git history, linked worktrees, and archived evidence. Do not discard ignored
artifacts without an explicit cleanup scope from the user. Track architecture and feature docs only;
`.gitignore` allows the existing guides plus `docs/architecture/` and `docs/features/`.
Add new standalone architecture/feature guides to that allowlist. Required license notices stay tracked.
Write testing, verification, benchmark, and deployment reports under `_work/reports/`, not tracked docs.
Legacy reports in `docs/` remain local and ignored. Do not force-add reports or link tracked guides
to local testing evidence. Historical report paths describe the recorded revisions.
