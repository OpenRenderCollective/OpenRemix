#!/usr/bin/env python3
"""Cross-build the Windows x64 runtime with clang-cl and the MSVC CRT/SDK (xwin),
with OpenUSD and DLSS, and collect it in a folder laid out as a game loads it."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import urllib.request

# The MSVC ABI is what NGX (DLSS) and an OpenUSD built for Windows link
# against; MinGW can build neither.
PINS = {
    'xwin': {'url': 'https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/'
                    'xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz',
             'sha256': 'd870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88'},
    'onetbb': {'url': 'https://github.com/uxlfoundation/oneTBB/archive/refs/tags/v2021.13.0.tar.gz',
               'sha256': '3ad5dd08954b39d113dc5b3f8a8dc6dc1fd5250032b7c491eb07aed5c94133e1'},
    'openusd': {'url': 'https://github.com/PixarAnimationStudios/OpenUSD/archive/refs/tags/v25.11.tar.gz',
                'sha256': 'c37c633b5037a4552f61574670ecca8836229b78326bd62622f3422671188667'},
    'json': {'url': 'https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp',
             'sha256': 'aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63'},
}
VULKAN_DEF = ('https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/v1.4.309/loader/vulkan-1.def',
              '9ba339b7f5ee2df28487698a6840ecf095fac58b415c2c89ad9f161e9d316378')


def download(url, digest, path):
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == digest:
        return
    data = urllib.request.urlopen(url).read()
    if hashlib.sha256(data).hexdigest() != digest:
        raise RuntimeError(f'{path.name}: checksum mismatch')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=root / '_work/build/windows')
    parser.add_argument('--deps', type=Path, default=root / '_work/deps/xwin')
    parser.add_argument('--output', type=Path, default=root / '_work/dist/windows/openremix',
                        help="the runtime's folder (a game's bin/win64/openremix); files are replaced, others kept")
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--tests', action='store_true', help='run the test suite under Wine after building')
    parser.add_argument('--accept-msvc-license', action='store_true',
                        help="accept Microsoft's license for the CRT and Windows SDK xwin downloads (first run)")
    args = parser.parse_args()
    build, deps, output = args.build.resolve(), args.deps.resolve(), args.output.resolve()
    for tool in ('clang-cl', 'lld-link', 'llvm-lib', 'llvm-rc', 'llvm-mt', 'meson', 'ninja', 'pkg-config',
                 'patch', args.cmake) + (('wine',) if args.tests else ()):
        if not shutil.which(tool):
            parser.error('Missing build tool: ' + tool)
    deps.mkdir(parents=True, exist_ok=True)
    prefix, sdk = deps / 'prefix', deps / 'root'
    (prefix / 'lib/pkgconfig').mkdir(parents=True, exist_ok=True)
    (prefix / 'bin').mkdir(parents=True, exist_ok=True)

    spec = importlib.util.spec_from_file_location('dependencies', root / 'scripts/common/build-dependencies.py')
    sys.dont_write_bytecode = True
    dependency_builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dependency_builder)
    fetch = dependency_builder.fetch

    # MSVC CRT and Windows SDK headers and import libraries.
    if not (sdk / 'crt/lib/x86_64').is_dir() or not (sdk / 'sdk/lib/um/x86_64').is_dir():
        if not args.accept_msvc_license:
            parser.error('The MSVC CRT and Windows SDK are downloaded from Microsoft by xwin; '
                         'pass --accept-msvc-license to accept their license')
        xwin = fetch(deps, 'xwin', PINS['xwin']) / 'xwin'
        subprocess.run([str(xwin), '--accept-license', '--cache-dir', str(deps / 'cache'),
                        'splat', '--output', str(sdk)], check=True)
    # Spellings the dependencies (OpenUSD) use that xwin's case symlinks miss
    # (the SDK's files are lowercase; Windows does not care, Linux does).
    for folder, names in (('sdk/include/um', ('Memoryapi.h', 'ShlwAPI.h', 'WinIoCtl.h', 'Winsock2.h')),
                          ('sdk/lib/um/x86_64', ('Ws2_32.lib', 'Dbghelp.lib', 'Psapi.lib', 'Shlwapi.lib'))):
        for name in names:
            alias = sdk / folder / name
            if not os.path.lexists(alias) and (sdk / folder / name.lower()).exists():
                alias.symlink_to(name.lower())
    includes = ' '.join('/imsvc' + str(sdk / p) for p in
                        ('crt/include', 'sdk/include/ucrt', 'sdk/include/shared', 'sdk/include/um', 'sdk/include/winrt'))
    compiler = deps / 'clang-cl'
    compiler.write_text(f'#!/bin/sh\nexec {shutil.which("clang-cl")} --target=x86_64-pc-windows-msvc {includes} "$@"\n')
    compiler.chmod(0o755)
    libraries = [sdk / 'crt/lib/x86_64', sdk / 'sdk/lib/um/x86_64', sdk / 'sdk/lib/ucrt/x86_64']
    toolchain = deps / 'clang-cl-xwin.cmake'
    toolchain.write_text(
        'set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_SYSTEM_PROCESSOR AMD64)\n'
        f'set(CMAKE_C_COMPILER "{compiler}")\nset(CMAKE_CXX_COMPILER "{compiler}")\n'
        'set(CMAKE_LINKER lld-link)\nset(CMAKE_AR llvm-lib)\nset(CMAKE_RC_COMPILER llvm-rc)\nset(CMAKE_MT llvm-mt)\n'
        'set(_libs "' + ' '.join('/libpath:' + str(p) for p in libraries) + '")\n'
        'set(CMAKE_EXE_LINKER_FLAGS_INIT "${_libs}")\nset(CMAKE_SHARED_LINKER_FLAGS_INIT "${_libs}")\n'
        'set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_libs}")\n'
        'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\nset(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n'
        'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\nset(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n'
        '# xwin provides only the release CRT: the release DLL runtime everywhere.\n'
        'set(CMAKE_TRY_COMPILE_CONFIGURATION Release)\nset(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)\n'
        'cmake_policy(SET CMP0091 NEW)\n')
    cmake = [args.cmake, '-G', 'Ninja', '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain), '-DCMAKE_BUILD_TYPE=Release']

    # Only an import library is needed: Windows/Proton provides vulkan-1.dll.
    # Match the repository's pinned Vulkan 1.4.309 headers.
    definition = deps / 'vulkan-1.def'
    download(*VULKAN_DEF, definition)
    subprocess.run(['llvm-lib', '/machine:x64', '/def:' + str(definition), '/out:' + str(prefix / 'lib/vulkan-1.lib')],
                   check=True, stdout=subprocess.DEVNULL)
    (prefix / 'lib/pkgconfig/vulkan.pc').write_text(
        'Name: Vulkan\nDescription: Vulkan Windows import library\nVersion: 1.4.309\n'
        f'Libs: {prefix}/lib/vulkan-1.lib\nCflags: -I{root}/include/vulkan/include\n')

    # Each dependency is built once; delete its install folder to rebuild it.
    if not (prefix / 'bin/SDL3.dll').exists() or not (prefix / 'lib/SDL3.lib').exists():
        pin = json.loads((root / 'scripts/common/dependencies.lock.json').read_text())['dependencies']['sdl3']
        source = fetch(deps, 'sdl3', pin)
        subprocess.run(cmake + ['-S', str(source), '-B', str(deps / 'build-sdl3'),
            '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
            '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST_LIBRARY=OFF',
            '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF'], check=True)
        subprocess.run([args.cmake, '--build', str(deps / 'build-sdl3'), '--parallel', str(args.jobs)], check=True)
        subprocess.run([args.cmake, '--install', str(deps / 'build-sdl3')], check=True)
    tbb = deps / 'tbb'
    if not (tbb / 'lib/cmake/TBB/TBBConfig.cmake').exists():
        source = fetch(deps, 'onetbb', PINS['onetbb'])
        subprocess.run(cmake + ['-S', str(source), '-B', str(deps / 'build-tbb'), '-DCMAKE_INSTALL_PREFIX=' + str(tbb),
            '-DTBB_TEST=OFF', '-DTBB_EXAMPLES=OFF', '-DTBB_STRICT=OFF', '-DTBB_DISABLE_HWLOC_AUTOMATIC_SEARCH=ON'],
            check=True)
        subprocess.run([args.cmake, '--build', str(deps / 'build-tbb'), '--parallel', str(args.jobs)], check=True)
        subprocess.run([args.cmake, '--install', str(deps / 'build-tbb')], check=True)
    usd = deps / 'usd'
    if not (usd / 'pxrConfig.cmake').exists():
        source = fetch(deps, 'openusd', PINS['openusd'])
        patched = source / '.remix-clang-cl'
        if not patched.exists():
            # clang-cl reports Clang but follows MSVC: take OpenUSD's MSVC paths.
            subprocess.run(['patch', '-p1', '-i', str(root / 'scripts/linux/openusd-25.11-clang-cl.patch')],
                           cwd=source, check=True)
            patched.touch()
        subprocess.run(cmake + ['-S', str(source), '-B', str(deps / 'build-usd'), '-DCMAKE_INSTALL_PREFIX=' + str(usd),
            '-DCMAKE_PREFIX_PATH=' + str(tbb), '-DTBB_ROOT=' + str(tbb), '-DBUILD_SHARED_LIBS=ON',
            '-DPXR_ENABLE_PYTHON_SUPPORT=OFF', '-DPXR_BUILD_IMAGING=OFF', '-DPXR_BUILD_USD_IMAGING=OFF',
            '-DPXR_BUILD_USDVIEW=OFF', '-DPXR_BUILD_TESTS=OFF', '-DPXR_BUILD_EXAMPLES=OFF',
            '-DPXR_BUILD_TUTORIALS=OFF', '-DPXR_BUILD_USD_TOOLS=OFF', '-DPXR_BUILD_USD_VALIDATION=OFF',
            '-DPXR_BUILD_EXEC=OFF', '-DPXR_BUILD_DOCUMENTATION=OFF', '-DPXR_ENABLE_MATERIALX_SUPPORT=OFF',
            '-DPXR_ENABLE_VULKAN_SUPPORT=OFF', '-DPXR_ENABLE_HDF5_SUPPORT=OFF', '-DPXR_ENABLE_OPENVDB_SUPPORT=OFF',
            '-DPXR_ENABLE_PTEX_SUPPORT=OFF', '-DPXR_ENABLE_OSL_SUPPORT=OFF'], check=True)
        subprocess.run([args.cmake, '--build', str(deps / 'build-usd'), '--parallel', str(args.jobs)], check=True)
        subprocess.run([args.cmake, '--install', str(deps / 'build-usd')], check=True)
    download(PINS['json']['url'], PINS['json']['sha256'], deps / 'json/nlohmann/json.hpp')

    wrapper = deps / 'wine-run.sh'
    wine_paths = ';'.join('Z:' + str(p).replace('/', '\\')
                          for p in (prefix / 'bin', tbb / 'bin', usd / 'lib', build / 'src/native'))
    wrapper.write_text('#!/usr/bin/env bash\nset -euo pipefail\n' +
        'export WINEPREFIX=' + shlex.quote(str(deps / 'wine-prefix')) + '\n' +
        'export WINEPATH=' + shlex.quote(wine_paths) + '\n' +
        'export WINEDEBUG=-all\nexport WINEDLLOVERRIDES="mscoree,mshtml="\n' + r'''args=()
for arg in "$@"; do
  if [[ "$arg" == /* ]]; then arg="Z:${arg//\//\\}"; fi
  args+=("$arg")
done
exec wine "${args[@]}"
''')
    wrapper.chmod(0o755)
    cross = deps / 'cross.ini'
    cross.write_text(f'''[binaries]
c = '{compiler}'
cpp = '{compiler}'
c_ld = 'lld-link'
cpp_ld = 'lld-link'
ar = 'llvm-lib'
strip = 'llvm-strip'
windres = 'llvm-rc'
pkg-config = 'pkg-config'
exe_wrapper = '{wrapper}'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '{prefix}/lib/pkgconfig'
''')
    # lld-link finds the CRT and SDK import libraries through LIB.
    env = dict(os.environ, LIB=';'.join(str(p) for p in libraries + [tbb / 'lib']))
    setup = ['meson', 'setup', str(build), str(root), '--cross-file', str(cross),
             '-Dbuildtype=release', '-Dwerror=true', '-Ddeps_root=' + str(prefix),
             '-Dusd=true', '-Dusd_root=' + str(usd), '-Dusd_tbb_root=' + str(tbb),
             '-Djson_include=' + str(deps / 'json'), '-Ddlss=true', '-Dgpu_tests=false',
             '-Denable_tests=' + ('true' if args.tests else 'false'),
             '-Dc_args=/Z7', '-Dcpp_args=/Z7', '-Dc_link_args=/DEBUG', '-Dcpp_link_args=/DEBUG']
    if (build / 'meson-private/coredata.dat').exists():
        setup += ['--reconfigure']
    subprocess.run(setup, check=True, env=env)
    subprocess.run(['meson', 'compile', '-C', str(build), '-j', str(args.jobs)], check=True, env=env)
    if args.tests:
        subprocess.run(['meson', 'test', '-C', str(build), '--print-errorlogs', '--num-processes', '1'],
                       check=True, env=env)

    # The runtime's folder: remix.dll and its DLLs side by side, OpenUSD's
    # plugins in usd/ beside usd_plug.dll, DLSS in dlss/<version>/.
    manifest = json.loads((build / 'src/native/upscale/dlss-runtime.json').read_text())
    if not manifest.get('enabled'):
        raise RuntimeError('The build has no DLSS')
    output.mkdir(parents=True, exist_ok=True)
    native = build / 'src/native'
    files = [native / 'remix.dll', native / 'remix.pdb', prefix / 'bin/SDL3.dll', tbb / 'bin/tbb12.dll']
    files += sorted((usd / 'lib').glob('usd_*.dll'))
    for path in files:
        shutil.copy2(path, output / path.name)
    shutil.copytree(usd / 'lib/usd', output / 'usd', dirs_exist_ok=True)
    runtime = output / 'dlss' / manifest['version']
    runtime.mkdir(parents=True, exist_ok=True)
    for name in ['nvngx_dlss.dll', 'nvngx_dlssd.dll', 'nvngx_dlssg.dll']:
        shutil.copy2(native / 'upscale' / name, runtime / name)
    shutil.copy2(root / 'third_party/dlss/LICENSE.txt', runtime / 'LICENSE.txt')
    # Ray Reconstruction's CNN model comes from an older release (ngx.cpp).
    cnn = output / 'dlss' / manifest['cnnVersion']
    cnn.mkdir(parents=True, exist_ok=True)
    shutil.copy2(native / 'upscale/cnn/nvngx_dlssd.dll', cnn / 'nvngx_dlssd.dll')
    shutil.copy2(root / 'third_party/dlss/LICENSE-310.2.1.txt', cnn / 'LICENSE.txt')
    # FSR: AMD's signed ffx-api DLLs in fidelityfx/ (upscale/ffx_library.cpp).
    fidelityfx = output / 'fidelityfx'
    fidelityfx.mkdir(parents=True, exist_ok=True)
    for name in ['amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll',
                 'amd_fidelityfx_framegeneration_vk.dll']:
        shutil.copy2(native / 'upscale' / name, fidelityfx / name)
    shutil.copy2(root / 'third_party/fidelityfx/FidelityFX-SDK/Kits/FidelityFX/docs/license.md',
                 fidelityfx / 'LICENSE.md')
    print('Windows runtime:', output)


if __name__ == '__main__':
    main()
