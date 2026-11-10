param([string]$Build = "_work/build/windows", [string]$Deps = "_work/deps/windows")
$ErrorActionPreference = "Stop"
# Run in a Visual Studio x64 developer PowerShell with Vulkan SDK installed.
$env:CC = "clang-cl"
$env:CXX = "clang-cl"
python "$PSScriptRoot/../common/build-dependencies.py" --output $Deps
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$prefix = (Resolve-Path "$Deps/prefix").Path
$env:PATH = "$prefix/bin;$env:PATH"
$oidn = (Resolve-Path "$Deps/oidn").Path
$setup = @("setup", $Build, "$PSScriptRoot/../..", "-Ddeps_root=$prefix", "-Doidn_root=$oidn", "-Dbuildtype=release", "-Dwerror=true")
if (Test-Path "$Build/meson-private/coredata.dat") { $setup += "--reconfigure" }
meson @setup
if ($LASTEXITCODE) { exit $LASTEXITCODE }
meson compile -C $Build
if ($LASTEXITCODE) { exit $LASTEXITCODE }
meson test -C $Build --print-errorlogs
exit $LASTEXITCODE
