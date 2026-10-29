#!/usr/bin/env python3
"""Package the shell, SDL3 and (on macOS Vulkan builds) standard SDK Vulkan/MoltenVK libraries."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--deps', type=Path, required=True, help='SDL build-dependencies output directory')
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--vulkan-sdk', type=Path)
parser.add_argument('--max-glibc', default='2.28', help='Linux package glibc ceiling')
parser.add_argument('--target', choices=['linux', 'windows', 'macos'], help='Override the host platform for cross-built packages')
args = parser.parse_args()
build, deps, output = args.build.resolve(), args.deps.resolve(), args.output.resolve()
if output.exists(): parser.error('Use a fresh output directory.')
system = {'linux': 'Linux', 'windows': 'Windows', 'macos': 'Darwin'}.get(args.target, platform.system())
# gpu_backend=metal builds (macOS) link neither Vulkan nor MoltenVK.
options = json.loads((build / 'meson-info/intro-buildoptions.json').read_text())
metal = any(o['name'] == 'gpu_backend' and o['value'] == 'metal' for o in options)
if system == 'Darwin' and not metal and not args.vulkan_sdk: parser.error('macOS Vulkan builds require --vulkan-sdk.')
for directory in ['bin', 'lib', 'include/remix', 'share/remix/licenses']:
    (output / directory).mkdir(parents=True, exist_ok=True)
licenses = output / 'share/remix/licenses'
for name in ['LICENSE', 'ThirdPartyLicenses.txt', 'README.md']:
    shutil.copy2(root / name, output / 'share/remix' / name)
for name in ['remix.h', 'remix_c.h', 'remix_native_ext.h', 'remix_reconstruction_ext.h', 'remix_sky_ext.h']:
    shutil.copy2(root / 'public/include/remix' / name, output / 'include/remix' / name)
shutil.copy2(root / 'third_party/imgui/LICENSE.txt', licenses / 'ImGui.txt')
shutil.copy2(root / 'third_party/image/LICENSES.txt', licenses / 'ImageDecoders.txt')
shutil.copy2(root / 'src/native/pathtrace/shaders/LICENSE-eon.txt', licenses / 'EON.txt')
for source, name in [('LICENSE.txt','NRD.txt'), ('External/MathLib/LICENSE.txt','MathLib.txt'),
                     ('External/ShaderMake/LICENSE.txt','ShaderMake.txt'), ('External/sse2neon/LICENSE','sse2neon.txt')]:
    shutil.copy2(root / 'third_party/nrd' / source, licenses / name)
shutil.copy2(root / 'third_party/nrd/README.remix.md', licenses / 'NRD-provenance.md')
shutil.copy2(root / 'third_party/sharc/License.md', licenses / 'SHARC.md')
shutil.copy2(root / 'third_party/sharc/README.md', licenses / 'SHARC-provenance.md')
numos = root / 'third_party/numos/numos-sky'
sky = any(o['name'] == 'sky' and o['value'] != 'disabled' for o in options)
if sky and (numos / 'NOTICE').exists():
    for source, name in [('LICENSE', 'NUMOS.txt'), ('NOTICE', 'NUMOS-NOTICE.txt'),
                         ('THIRD_PARTY_NOTICES.md', 'NUMOS-THIRD_PARTY_NOTICES.md')]:
        shutil.copy2(numos / source, licenses / name)
    for name in ['LICENSE-MIT-NVIDIA.txt', 'LICENSE-MIT-contributors.txt',
                 'LICENSE-MIT-Blender-sky.txt', 'LICENSE-DXVK-zlib.txt']:
        shutil.copy2(numos / 'licenses' / name, licenses / ('NUMOS-' + name))
    shutil.copy2(root / 'third_party/numos/README.remix.md', licenses / 'NUMOS-provenance.md')
if not metal:
    shutil.copytree(root / 'include/vulkan/LICENSES', licenses / 'Vulkan-Headers')
    shutil.copy2(root / 'include/vulkan/LICENSE.md', licenses / 'Vulkan-Headers/NOTICE.md')
    # Windows ships FidelityFX SDK 2.4.0's DLLs and license (below); the other
    # platforms link FSR 3.1.4.
    if system != 'Windows':
        shutil.copy2(root / 'third_party/fsr31/LICENSE.txt', licenses / 'FidelityFX.txt')
else:
    shutil.copy2(root / 'third_party/xz-embedded/COPYING', licenses / 'XZEmbedded.txt')
shutil.copy2(deps / 'sources/sdl3/LICENSE.txt', licenses / 'SDL3.txt')
shutil.copy2(root / 'scripts/common/dependencies.lock.json', output / 'share/remix/dependencies.lock.json')
if system == 'Windows':
    for path in [build / 'src/native/remix.dll', deps / 'prefix/bin/SDL3.dll',
                 build / 'samples/remix-native/remix-native-sample.exe']:
        shutil.copy2(path, output / 'bin' / path.name)
    for path in (deps / 'prefix/bin').glob('lib*.dll'):
        shutil.copy2(path, output / 'bin' / path.name)
    # FSR: AMD's signed ffx-api DLLs, loaded from fidelityfx/ beside remix.dll
    # (src/native/upscale/ffx_library.cpp), with the license they require.
    fidelityfx = root / 'third_party/fidelityfx/FidelityFX-SDK/Kits/FidelityFX'
    (output / 'bin/fidelityfx').mkdir()
    for name in ['amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll',
                 'amd_fidelityfx_framegeneration_vk.dll']:
        shutil.copy2(fidelityfx / 'signedbin' / name, output / 'bin/fidelityfx' / name)
    shutil.copy2(fidelityfx / 'docs/license.md', output / 'bin/fidelityfx/LICENSE.md')
    shutil.copy2(fidelityfx / 'docs/license.md', licenses / 'FidelityFX-SDK.md')
    # XeSS: Intel's signed libxess.dll, loaded from xess/ beside remix.dll
    # (src/native/upscale/xess.cpp), with the notices its license requires.
    xess = root / 'third_party/xess/xess'
    (output / 'bin/xess').mkdir()
    shutil.copy2(xess / 'bin/libxess.dll', output / 'bin/xess/libxess.dll')
    for name in ['LICENSE.txt', 'third-party-programs.txt']:
        shutil.copy2(xess / name, output / 'bin/xess' / name)
    shutil.copy2(xess / 'LICENSE.txt', licenses / 'XeSS.txt')
elif system == 'Linux':
    staging = tempfile.TemporaryDirectory(prefix='remix-package-')
    subprocess.run(['meson', 'install', '-C', str(build), '--no-rebuild', '--destdir', staging.name],
                   check=True, stdout=subprocess.DEVNULL)
    installed = next(Path(staging.name).rglob('libremix.so.0.1003.0')).parent
    for path in list(installed.glob('libremix.so*')) + list((deps / 'prefix/lib').glob('libSDL3.so*')):
        if path.is_symlink():
            (output / 'lib' / path.name).symlink_to(path.resolve().name)
        elif path.is_file():
            subprocess.run(['bash', str(root / 'scripts/linux/check-glibc.sh'), str(path), args.max_glibc], check=True)
            shutil.copy2(path, output / 'lib' / path.name)
    sample = next(Path(staging.name).rglob('remix-native-sample'))
    subprocess.run(['bash', str(root / 'scripts/linux/check-glibc.sh'), str(sample), args.max_glibc], check=True)
    shutil.copy2(sample, output / 'bin' / sample.name)
    for path in (output / 'lib').iterdir():
        if path.is_symlink(): continue
        dynamic = subprocess.check_output(['readelf', '-d', path], text=True)
        needed = '\n'.join(line.lower() for line in dynamic.splitlines() if '(NEEDED)' in line)
        for removed in ['ngx', 'nrd', 'usd', 'rtxio', 'dxvk', 'd3d', 'xess', 'slang']:
            if removed in needed: raise RuntimeError(f'Removed dependency {removed}: {path}')
    launcher = output / 'bin/run-sample'
    launcher.write_text('''#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
export LD_LIBRARY_PATH="$root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$root/bin/remix-native-sample" "$root/lib/libremix.so.0" "$@"
''')
    launcher.chmod(0o755)
elif system == 'Darwin':
    libraries = {
        'libremix.0.dylib': build / 'src/native/libremix.0.dylib',
        'libSDL3.0.dylib': deps / 'prefix/lib/libSDL3.0.dylib',
    }
    if not metal:
        sdk = args.vulkan_sdk.resolve()
        libraries['libvulkan.1.dylib'] = sdk / 'lib/libvulkan.1.dylib'
        libraries['libMoltenVK.dylib'] = sdk / 'lib/libMoltenVK.dylib'
    for name, source in libraries.items():
        target = output / 'lib' / name
        architectures = subprocess.check_output(['lipo', '-archs', source], text=True).split()
        if 'arm64' not in architectures: raise RuntimeError(f'No arm64 slice: {source}')
        if len(architectures) > 1:
            subprocess.run(['lipo', source, '-thin', 'arm64', '-output', target], check=True)
        else: shutil.copy2(source, target)
        subprocess.run(['install_name_tool', '-id', '@rpath/' + name, target], check=True)
        for line in subprocess.check_output(['otool', '-L', target], text=True).splitlines()[2:]:
            dependency = line.strip().split(' (compatibility version', 1)[0]
            if dependency.startswith(('/usr/lib/', '/System/Library/')): continue
            basename = Path(dependency).name
            if basename not in libraries: raise RuntimeError(f'Unbundled dependency: {dependency}')
            subprocess.run(['install_name_tool', '-change', dependency, '@loader_path/' + basename, target], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', target], check=True)
    shutil.copy2(build / 'samples/remix-native/remix-native-sample', output / 'bin/remix-native-sample')
    (output / 'lib/libremix.dylib').symlink_to('libremix.0.dylib')
    launcher = output / 'bin/run-sample'
    if metal:
        launcher.write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
import sys
root = Path(__file__).resolve().parent.parent
env = dict(os.environ, DYLD_LIBRARY_PATH=str(root / 'lib'))
os.execve(root / 'bin/remix-native-sample', ['remix-native-sample', str(root / 'lib/libremix.0.dylib'), *sys.argv[1:]], env)
''')
        launcher.chmod(0o755)
if system == 'Darwin' and not metal:
    manifests = list((sdk / 'share/vulkan/icd.d').glob('*MoltenVK*.json')) + list((sdk / 'etc/vulkan/icd.d').glob('*MoltenVK*.json'))
    if not manifests: raise RuntimeError('SDK MoltenVK ICD manifest missing')
    icd = json.loads(manifests[0].read_text())
    icd['ICD']['library_path'] = '../../../lib/libMoltenVK.dylib'
    target = output / 'etc/vulkan/icd.d/MoltenVK_icd.json'
    target.parent.mkdir(parents=True)
    target.write_text(json.dumps(icd, indent=2) + '\n')
    sdk_licenses = next((p for p in [sdk.parent / 'Licenses', sdk / 'share/vulkan/licensing', sdk / 'Licenses'] if p.is_dir()), None)
    if sdk_licenses is None: raise RuntimeError('Vulkan SDK license directory missing')
    shutil.copytree(sdk_licenses, licenses / 'VulkanSDK')
    launcher.write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
import sys
root = Path(__file__).resolve().parent.parent
env = dict(os.environ, VK_DRIVER_FILES=str(root / 'etc/vulkan/icd.d/MoltenVK_icd.json'),
           DYLD_LIBRARY_PATH=str(root / 'lib'))
os.execve(root / 'bin/remix-native-sample', ['remix-native-sample', str(root / 'lib/libremix.0.dylib'), *sys.argv[1:]], env)
''')
    launcher.chmod(0o755)
if system not in ('Windows', 'Linux', 'Darwin'):
    parser.error('Use scripts/android/build-apk.py for Android packaging.')
# Package the actual build's optional NGX runtime, never an installed version.
manifest_path = build / 'src/native/upscale/dlss-runtime.json'
manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {'enabled': False}
if manifest['enabled'] and system in ('Linux', 'Windows'):
    isolated = output / ('lib' if system == 'Linux' else 'bin') / 'dlss' / manifest['version']
    isolated.mkdir(parents=True)
    binaries = ['libnvidia-ngx-dlss.so.310.9.1', 'libnvidia-ngx-dlssd.so.310.9.1'] if system == 'Linux' else ['nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll']
    release = 'lib/Linux_x86_64/rel' if system == 'Linux' else 'lib/Windows_x86_64/rel'
    import hashlib
    expected = {name: digest for digest, name in
                (line.split('  ', 1) for line in (root / 'third_party/dlss/sources.sha256').read_text().splitlines())}
    def copy_checked(source, vendored, target):
        if hashlib.sha256(source.read_bytes()).hexdigest() != expected[vendored]:
            raise RuntimeError('DLSS release library hash mismatch: ' + vendored)
        shutil.copy2(source, target)
    for name in binaries:
        copy_checked(build / 'src/native/upscale' / name, release + '/' + name, isolated / name)
    for name in ('LICENSE.txt', 'README.remix.md', 'upstream.json', 'sources.sha256'):
        shutil.copy2(root / 'third_party/dlss' / name, isolated / name)
    # Ray Reconstruction's CNN model comes from an older release (ngx.cpp).
    cnn = isolated.parent / manifest['cnnVersion']
    cnn.mkdir(parents=True)
    name = 'libnvidia-ngx-dlssd.so.310.2.1' if system == 'Linux' else 'nvngx_dlssd.dll'
    copy_checked(build / 'src/native/upscale/cnn' / name, release + '-310.2.1/' + name, cnn / name)
    shutil.copy2(root / 'third_party/dlss/LICENSE-310.2.1.txt', cnn / 'LICENSE.txt')
    shutil.copy2(root / 'third_party/dlss/LICENSE.txt', licenses / 'DLSS.txt')
print(output)
