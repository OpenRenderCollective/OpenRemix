# Platform builds and packages

All builds use Meson/Ninja and Clang. Initialize the `include/vulkan` submodule.
SDL3 remains pinned to 3.4.14; `scripts/common/build-dependencies.py` verifies the
archive checksum and builds only SDL3. Dear ImGui v1.92.9b is vendored with checksums.
PNG/DDS decoders are included in the source tree. The build generates the renderer's
SPIR-V (and on Metal builds its libraries) with glslang and SPIRV-Tools at the revisions in
`scripts/shaders/toolchain.lock.json`: the first build on a machine fetches them with Git and
builds them with CMake for the build machine (about a minute), cached in
`_work/tools/shader-toolchain` for every build directory (`-Dshader_tools=` moves the cache).
None of this adds a runtime dependency.
Use fresh build directories when switching from the removed renderer.

## Linux x64 / glibc 2.28

Use Rocky Linux 8 (or a compatible sysroot) for portable binaries. Building on a newer
distribution does not establish a glibc 2.28 baseline. The CI job builds and packages
inside Rocky Linux 8 and rejects runtime, SDL3 or sample binaries requiring newer glibc.
Clang, Python 3.9+, Meson, CMake, Ninja, Git, Vulkan loader development files and SDL platform
headers are required. The system supplies libvulkan and the GPU driver.

```sh
python3 scripts/common/build-dependencies.py --output _work/deps/linux
scripts/linux/build-remix.sh _work/build/linux _work/deps/linux/prefix
python3 scripts/common/package-remix.py --build _work/build/linux --deps _work/deps/linux --output _work/packages/linux
_work/packages/linux/bin/run-sample 300
```

The package includes the versioned `libremix.so.0.1003.0`, `.so.0` and `.so` aliases,
SDL3, the sample, SDK headers, pin and license notices. No Vulkan driver is bundled on Linux.

## Windows x64

### Cross-build from Linux

Linux hosts build with clang-cl against the MSVC CRT and Windows SDK, which
[xwin](https://github.com/Jake-Shadle/xwin) downloads from Microsoft, plus Python,
CMake, Meson, Ninja, pkg-config, LLVM (lld-link, llvm-lib, llvm-rc, llvm-mt), patch
and, for the tests, Wine:

Arch Linux specific packages: `sudo pacman -S --needed clang llvm lld meson ninja cmake pkgconf python patch wine git`

```sh
python3 scripts/linux/build-windows-remix.py --accept-msvc-license   # the first run
python3 scripts/linux/build-windows-remix.py --output /path/to/game/bin/win64/openremix
```

The build has OpenUSD and DLSS (both need the MSVC ABI) and FSR. On its first run
the script splats the CRT and SDK (accepting Microsoft's license is up to you, hence
the flag), then builds the pinned SDL3, oneTBB 2021.13.0 and OpenUSD 25.11 (patched
for clang-cl, `scripts/linux/openusd-25.11-clang-cl.patch`) into `_work/deps/xwin`;
later runs reuse them. It collects the runtime in `--output` (default
`_work/dist/windows/openremix`) as a game loads it: `remix.dll` and its PDB, SDL3,
TBB and the `usd_*.dll` side by side, OpenUSD's plugins in `usd/`, DLSS in
`dlss/310.9.1/`, Ray Reconstruction's CNN model in `dlss/310.2.1/` and AMD's FSR libraries in
`fidelityfx/`. Files already in the folder that the build does not make (profiles,
caches, logs) are kept. `--tests` runs the CPU tests in an isolated Wine prefix.
Use `--cmake /path/to/cmake` if CMake is outside PATH. Windows or Proton supplies the
Vulkan loader, the GPU driver and the Visual C++ runtime (`vcruntime140.dll`,
`msvcp140.dll`).

### Build on Windows

Install the Vulkan SDK and Clang, and run in a Visual Studio x64 developer PowerShell.
The compiler is clang-cl with the Visual Studio headers, import libraries and CRT.
Install `meson`, `ninja` and `cmake` through Python or your tool installer.
Windows builds need the FidelityFX SDK submodule for FSR:
`git submodule update --init third_party/fidelityfx/FidelityFX-SDK`.

```powershell
./scripts/windows/build-remix.ps1
python scripts/common/package-remix.py --build _work/build/windows --deps _work/deps/windows --output _work/packages/windows
./_work/packages/windows/bin/remix-native-sample.exe
```

The package contains `remix.dll`, `SDL3.dll` and the sample together in `bin`, and AMD's FSR
libraries (loader, upscaler, frame generation) in `bin/fidelityfx`. The loader picks FSR 4 on
RDNA 4 GPUs and FSR 3.1 elsewhere. For testing, `REMIX_FSR_VERSION=3` asks the libraries for
their FSR 3.1 provider, and builds configured with `-Dfsr_linked=true` also contain the linked
FSR 3.1.4 that Linux and macOS use, which `REMIX_FSR_LINKED=1` selects.
The Vulkan loader/driver and Visual C++ runtime are platform prerequisites. Null HWND
startup creates a 1280×720 window. Existing HWNDs retain native ownership after Shutdown.
GPU tests (`meson configure _work/build/windows -Dgpu_tests=true`) include owned/borrowed HWNDs
and rejection of alternate presentation targets. Hosted Windows CI builds and runs CPU
checks; interactive Vulkan/borrowed-window checks require a suitable Windows machine.

## macOS arm64

Use Apple Clang, Meson, Ninja, CMake and the standard Vulkan SDK. Pass its macOS directory
(the one containing `bin`, `lib`, `share`) as `--vulkan-sdk`.

```sh
python3 scripts/common/build-dependencies.py --output _work/deps/macos
python3 scripts/macos/build-remix.py --vulkan-sdk "$VULKAN_SDK"
python3 scripts/common/package-remix.py --build _work/build/macos --deps _work/deps/macos \
  --vulkan-sdk "$VULKAN_SDK" --output _work/packages/macos
_work/packages/macos/bin/run-sample 300
```

Packaging uses SDK `libMoltenVK.dylib` and `libvulkan.1.dylib`, rewrites local library
references, signs copied libraries ad hoc, and installs a relative ICD manifest. The
runtime names remain `libremix.0.dylib` and `libremix.dylib`. Startup runs on the main
thread. The deployment target for authored code is macOS 13; the selected SDK's binary
minimum OS requirement also applies.

### Native Metal backend

`-Dgpu_backend=metal` builds a separate runtime that renders with Metal directly and links
neither Vulkan nor MoltenVK; the backend is chosen at compile time and one build contains one
backend. It needs Xcode's Command Line Tools and no Vulkan SDK. Path tracing requires a Metal
ray tracing device (Apple7 GPU family or newer). MetalFX replaces FSR and DLSS: upscaling, and on
macOS 26 denoising and frame generation. See [the Metal backend](features/metal-backend.md).

```sh
python3 scripts/common/build-dependencies.py --output _work/deps/macos
python3 scripts/macos/build-remix.py --metal
python3 scripts/common/package-remix.py --build _work/build/macos-metal --deps _work/deps/macos \
  --output _work/packages/macos-metal
_work/packages/macos-metal/bin/run-sample 300 pathtrace
```

The Metal package contains `libremix`, SDL3 and the sample only. GPU tests
(`-Dgpu_tests=true`) add rendering tests of raster, path traced, NRD and MetalFX frames, and of
halos, masks, cut coverage and soft sprites.

The separately pinned experimental driver, SPIRV-Cross revisions and patches remain
in `tools/moltenvk/dependencies.lock.json`, `build-moltenvk.py` and `patches/`.
`tools/vulkan-probes/macos-run-probe.py` requires explicit experimental GPU opt-in because of the historical
renderer/driver findings; normal shell build and run paths use SDK MoltenVK.

## Android arm64

Use Android NDK r29, SDK platform 35, build-tools 35.0.0, Java and Python. The minimum
API level is 29. Cross-built CPU/ABI tests are compiled; execute device tests on Android.

```sh
python3 scripts/common/build-dependencies.py --ndk /path/to/android-ndk-r29 --output _work/deps/android
python3 scripts/android/build-remix.py --ndk /path/to/android-ndk-r29
python3 scripts/android/build-apk.py --ndk /path/to/android-ndk-r29 --sdk /path/to/android-sdk
python3 scripts/android/run-sample.py --frames 5000 --test-lifecycle
```

`--validation-layer /path/to/libVkLayer_khronos_validation.so` on `build-apk.py` packages
an arm64 validation layer; `run-sample.py --validation` requests it. The APK contains
only `libmain.so`, `libremix.so`, `libSDL3.so`, optional validation, and SDL/Remix Java
activity classes. The sample always enables the debug panel and writes no renderer
presets. SDL forwards touch and text input to the panel. Background/resume recreates
stale Android Vulkan surfaces before presenting again.

Packages use fresh output directories under `_work/packages/`. Build and dependency
outputs belong under `_work/build/` and `_work/deps/`; `ref/` and archived local
artifacts are not package inputs.
