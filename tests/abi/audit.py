"""Check retained headers, ImGui pin, exports and absence of renderer dependencies."""
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import sys

root, library = Path(sys.argv[1]), Path(sys.argv[2])
for manifest, base in [(root / 'tests/abi/headers.sha256', root),
                       (root / 'third_party/imgui/SHA256SUMS', root / 'third_party/imgui'),
                       (root / 'third_party/image/SHA256SUMS', root / 'third_party/image'),
                       (root / 'third_party/nrd/sources.sha256', root / 'third_party/nrd'),
                       (root / 'third_party/sharc/sources.sha256', root / 'third_party/sharc'),
                       (root / 'third_party/fsr31/sources.sha256', root / 'third_party/fsr31'),
                       (root / 'third_party/dlss/sources.sha256', root / 'third_party/dlss'),
                       (root / 'third_party/xz-embedded/sources.sha256', root / 'third_party/xz-embedded')]:
    for line in manifest.read_text().splitlines():
        checksum, filename = line.split('  ', 1)
        assert hashlib.sha256((base / filename).read_bytes()).hexdigest() == checksum, filename
# A submodule has no checksum manifest: its gitlink and checkout must match the pin.
numos = json.loads((root / 'third_party/numos/upstream.json').read_text())
fidelityfx = json.loads((root / 'third_party/fidelityfx/upstream.json').read_text())
for name, pin in [('NUMOS', numos), ('FidelityFX SDK', fidelityfx)]:
    if not (root / '.git').exists():
        break
    gitlink = subprocess.check_output(['git', '-C', root, 'ls-files', '-s', pin['path']], text=True).split()
    assert gitlink[:2] == ['160000', pin['revision']], name + ' gitlink differs from upstream.json'
    if (root / pin['path'] / '.git').exists():
        head = subprocess.check_output(['git', '-C', root / pin['path'], 'rev-parse', 'HEAD'], text=True).strip()
        assert head == pin['revision'], name + ' checkout differs from upstream.json; run git submodule update'
# Apache-2.0 4(d): ThirdPartyLicenses.txt reproduces the NUMOS NOTICE.
notice = root / numos['path'] / 'NOTICE'
if notice.exists():
    notices = (root / 'ThirdPartyLicenses.txt').read_text(encoding='utf-8')
    for line in notice.read_text(encoding='utf-8').splitlines():
        assert line.strip() in notices, 'ThirdPartyLicenses.txt lacks NUMOS NOTICE line: ' + line
required_exports = (root / 'tests/abi/exports.txt').read_text().splitlines()
required_exports += ['remixapi_GetReconstructionDiagnosticsEXT', 'remixapi_GetBackendCapabilitiesEXT', 'remixapi_GetTracingStatisticsEXT', 'remixapi_GetFramePerformanceEXT', 'remixapi_GetFrameBreakdownEXT', 'remixapi_GetGpuPassTimingsEXT',
             'remixapi_GetLocalLightGuidingStatisticsEXT', 'remixapi_GetLocalLightStatisticsEXT',
             'remixapi_GetLocalLightSetStatisticsEXT', 'remixapi_GetCameraCullingStatisticsEXT',
             'remixapi_GetShadowStatisticsEXT', 'remixapi_GetSharcStatisticsEXT',
             'remixapi_GetDenoisingCapabilitiesEXT', 'remixapi_GetDenoisingStatisticsEXT',
             'remixapi_SetSkyTimeEXT', 'remixapi_SetSkyWeatherPresetEXT', 'remixapi_SetSkyWeatherFieldEXT',
             'remixapi_ClearSkyWeatherEXT', 'remixapi_TriggerSkyLightningEXT', 'remixapi_GetSkyStateEXT']
required_exports += ['remixapi_AddSceneInstancesEXT', 'remixapi_SetSceneInstanceEXT', 'remixapi_ChangeSceneInstancesEXT',
             'remixapi_RemoveSceneInstancesEXT', 'remixapi_DrawSceneInstancesEXT']
required_exports += ['remixapi_UpdateTextureEXT', 'remixapi_GetTextureStatisticsEXT']
required_exports += ['remixapi_CreateParticleEmitterEXT', 'remixapi_UpdateParticleEmitterEXT',
             'remixapi_SetParticleEmitterTransformEXT', 'remixapi_SetParticleEmitterEnabledEXT',
             'remixapi_BurstParticleEmitterEXT', 'remixapi_ClearParticleEmitterEXT',
             'remixapi_DestroyParticleEmitterEXT', 'remixapi_SetParticleBudgetEXT', 'remixapi_GetParticleStatisticsEXT']
required_exports += ['remixapi_EditorConfigureSharedFrame', 'remixapi_LoadUSDSceneEXT', 'remixapi_DrawUSDSceneEXT', 'remixapi_ReloadUSDSceneEXT', 'remixapi_DestroyUSDSceneEXT', 'remixapi_GetUSDDiagnosticsEXT', 'remixapi_ConfigureUSDModsEXT', 'remixapi_GetUSDModsDiagnosticEXT']
cross_windows = library.suffix.lower() == '.dll' and sys.platform != 'win32'
if cross_windows:
    # The host Python cannot load a PE DLL. Inspect it here; the Windows export
    # test executes loader/widget checks through Meson's Wine exe_wrapper.
    pe = subprocess.check_output(['objdump', '-p', library], text=True)
    exported = {line.split()[-1] for line in pe.splitlines() if 'remixapi_' in line}
    for name in required_exports:
        assert name in exported, name
    needed = '\n'.join(line.lower() for line in pe.splitlines() if 'DLL Name:' in line)
    for removed in ['ngx', 'nrd', 'rtxio', 'dxvk', 'd3d', 'xess', 'slang'] + ([] if '--usd' in sys.argv else ['usd']):
        assert removed not in needed, removed
else:
    runtime = ctypes.CDLL(str(library), **({'winmode': 0} if sys.platform == 'win32' else {}))
    for name in required_exports:
        assert getattr(runtime, name), name
    assert runtime.remixapi_imgui_Begin(None, None, 0) == 0
    value = ctypes.c_float(12)
    runtime.remixapi_imgui_GetWindowSize(ctypes.byref(value), None)
    assert value.value == 0
if sys.platform.startswith('linux') and not cross_windows:
    dynamic = subprocess.check_output(['readelf', '-d', library], text=True)
    needed = '\n'.join(line.lower() for line in dynamic.splitlines() if '(NEEDED)' in line)
    for removed in ['ngx', 'nrd', 'rtxio', 'dxvk', 'd3d', 'xess', 'slang'] + ([] if '--usd' in sys.argv else ['usd']):
        assert removed not in needed, removed
    symbols = subprocess.check_output(['nm', '-D', '--undefined-only', library], text=True)
    assert 'vkGetBufferDeviceAddress' not in symbols, 'Tracing must not require a Vulkan 1.2 loader for raster'
print('Header checksums, vendor pins, required exports and dependency audit passed')
