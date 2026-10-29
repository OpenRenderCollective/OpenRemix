# Forward+ setup from a fresh clone

This guide requires only the two public repositories plus the toolchains below.
Choose any writable parent directory; no owner-local drive layout is assumed.
`_work` paths below are outputs created by the build, not files to obtain from
another developer. Read each repository's AGENTS.md where present.

## Check out the branches

```sh
git clone --branch forward-plus-renderer https://github.com/OpenRenderCollective/OpenRemix.git OpenRemix
git clone --branch forward-plus-renderer https://github.com/sambow23/garrys-mod-rtx-remixed-2.git garrys-mod-rtx-remixed-2
git -C OpenRemix submodule update --init third_party/numos/numos-sky
```

The last command initializes the optional pinned sky dependency without recursion.
`-Dsky=auto` can build without it. Vulkan headers are already committed; there is
no Vulkan submodule to initialize. Public ABI headers are in `public/include`.

## Native runtime on Windows x64

Use a Visual Studio x64 developer PowerShell with the C++ toolchain/Windows SDK,
Clang (`clang-cl`), Vulkan SDK, Python, Git, Meson 1.3 or newer, Ninja and CMake
available. The Vulkan driver is supplied by the machine. From the native clone:

```powershell
Set-Location OpenRemix
./scripts/windows/build-remix.ps1
```

The tracked script builds pinned SDL3 into `_work/deps/windows`, builds the runtime
in `_work/build/windows`, then runs enabled tests. Shader tools are fetched and
built automatically at pinned revisions. First-time builds need network access;
no generated shaders or private build wrapper are required. To choose output
locations, the same tracked script accepts:

```powershell
./scripts/windows/build-remix.ps1 -Build _work/build/my-forward -Deps _work/deps/windows
```

Run it from the clone root. Use one build directory per compiler/toolchain. It
reconfigures an existing build and propagates build/test failures to the caller.

To enable hardware tests after that baseline:

```powershell
meson configure _work/build/windows -Dgpu_tests=true
meson compile -C _work/build/windows
meson test -C _work/build/windows --list
meson test -C _work/build/windows --print-errorlogs --num-processes 1
```

GPU cases require a compatible Vulkan device and SDL display environment; ray-query
cases additionally require hardware support. Keep the built SDL prefix's `bin` on
PATH in a fresh shell. Enable validation only with the validation layer installed.

## Native runtime on Linux

Install Clang, Python, Git, Meson, Ninja, CMake, Vulkan loader development files
and SDL platform development dependencies. From `OpenRemix/`:

```sh
python3 scripts/common/build-dependencies.py --output _work/deps/linux
scripts/linux/build-remix.sh _work/build/linux _work/deps/linux/prefix
```

See [platform builds](../platforms.md) for portability requirements and the tracked
Linux-to-Windows cross-build entry point. Do not assume a Linux runtime can be
loaded by the Windows game proxy.

## Optional runtime features

The baseline does not include OpenUSD replacement support or an external Open Image
Denoise SDK. For those, install compatible dependencies and configure your own
prefixes using `usd`, `usd_root`, `json_include`, `usd_tbb_root` and `oidn_root` in
`meson_options.txt`. See [platform builds](../platforms.md). Do not copy an owner's
absolute SDK paths. A baseline without these features is not equivalent to a
replacement-enabled, denoised playable package.

Example Windows configuration after the baseline build (replace every SDK path):

```powershell
meson configure _work/build/windows -Dusd=true `
  -Dusd_root=C:/SDKs/OpenUSD-25.11 -Djson_include=C:/SDKs/json-3.12/include `
  -Dusd_tbb_root=C:/SDKs/TBB -Doidn_root=C:/SDKs/oidn-2.5
meson compile -C _work/build/windows
meson test -C _work/build/windows --print-errorlogs --num-processes 1
```

The TBB build must match OpenUSD. These are SDK prefixes, not DLL paths. SDKs are
not fetched by the baseline SDL dependency helper. Package only after the build
and relevant checks pass.

## Build the Garry's Mod adapter

The adapter is a Windows x64 proxy with a separate toolchain. Its tracked Linux
cross file uses MinGW-w64 GCC, not clang-cl. Install that toolchain, Meson/Ninja
and `glslangValidator` on PATH. After building native SDL dependencies on Linux,
run from `garrys-mod-rtx-remixed-2/` (sibling clone layout):

```sh
native=$(realpath ../OpenRemix)
meson setup _work/build/mingw --cross-file cross/x86_64-w64-mingw32.txt \
  "-Dopenremix_include=$native/public/include" \
  "-Dsdl_include=$native/_work/deps/linux/prefix/include"
meson compile -C _work/build/mingw
```

For the same cross-build, the adapter includes a convenience helper. It resolves
its own repository location, checks required headers and tools, and defaults to
four compile jobs:

```sh
python3 tools/build_proxy.py --dry-run
python3 tools/build_proxy.py --jobs 4
```

For a different clone layout or SDL installation:

```sh
python3 tools/build_proxy.py --native-root /path/to/OpenRemix \
  --sdl-include /path/to/SDL/include --build _work/build/mingw --jobs 4
```

The helper requires Linux for execution; a dry run works elsewhere if headers are
available. It downloads nothing, runs no game, and does not deploy. Repeated calls
reconfigure an existing build. Relative explicit paths resolve from the caller's
working directory; default paths resolve from the repository location.

Both include options in the manual Meson recipe are required for this layout. They supply headers; the adapter
loads the Windows runtime at run time. Native Windows MinGW builds need equivalent
compiler and include settings. The native shader-tool cache does not automatically
put `glslangValidator` on the adapter's PATH.

## Package and test with your own game

The native repository has tracked `scripts/common/package-remix.py` and
`scripts/windows/package-runtime.py`. For the Windows build above, from the native
clone, create a fresh flat runtime package with:

```powershell
python scripts/windows/package-runtime.py --build _work/build/windows `
  --sdl _work/deps/windows/prefix/bin/SDL3.dll `
  --output _work/packages/windows-runtime
```

Use the actual SDL DLL from your build if its filename differs. The destination
must be new; the packager reads configured SDK locations and gathers enabled
runtime dependencies. It does not install into a game. Inspect `--help` for other
layouts. The general packager also includes development headers and samples.
The adapter has tracked `tools/stage_game.py` and `tools/deploy_testing.py`.
Use your own legally installed game, original shader DLL and generated package.
Read the adapter README before installing; prefer an isolated test installation.

`deploy_testing.py` takes your own settings JSON with `source`, `target`, `proxy`,
`package` and `native_repo` paths. The owner's `_work/testing-install.json` is not
part of the handoff. `launch_testing.ps1` is intended to run from the staged game
where it is installed, not directly from the repository's `tools` directory.
Preserve user settings, mods and authored cubemaps when updating a playable install.

Native policy and GPU fixture tests do not require a Garry's Mod installation.
Live map tests, replay inputs and sky captures require your own assets. The adapter
tracks a Construct cubemap reference JSON; private study maps, PBR materials,
reports, bake caches and old packages are not prerequisites or distributed assets.

These commands were checked against tracked scripts and Meson options for this
handoff; a clean-machine build has not been executed as part of the documentation
update. Record actual build/test results separately under `_work/reports`.
