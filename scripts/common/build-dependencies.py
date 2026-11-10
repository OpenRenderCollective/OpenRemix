#!/usr/bin/env python3
"""Build the pinned SDL3 shared library, and on Windows fetch the pinned Open Image
Denoise SDK; Vulkan comes from the platform SDK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile

# Intel's prebuilt Open Image Denoise SDK for Windows x64 (-Doidn_root):
# lightmap denoising, with its CPU, CUDA, HIP and SYCL devices.
OIDN_WINDOWS = {'url': 'https://github.com/RenderKit/oidn/releases/download/v2.5.1/oidn-2.5.1.x64.windows.zip',
                'sha256': 'f11f91bc072a5e3a564515724cb72ab8fcfbc445c84a84c197ff9b16cc01396f'}

def fetch(output, name, pin, strip_root=True):
    archive = output / (name + '.tar.gz')
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != pin['sha256']:
        temporary = archive.with_suffix('.download')
        urllib.request.urlretrieve(pin['url'], temporary)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != pin['sha256']:
            raise RuntimeError(f'{name}: archive checksum mismatch')
        temporary.replace(archive)
    destination = output / 'sources' / name
    marker = destination / '.remix-pin'
    if marker.exists() and marker.read_text() == pin['sha256']:
        return destination
    if destination.exists():
        raise RuntimeError(f'{destination} exists without the matching source pin; use a fresh output directory')
    destination.mkdir(parents=True)
    with tarfile.open(archive) as source:
        for member in source.getmembers():
            if strip_root:
                parts = member.name.split('/', 1)
                if len(parts) != 2 or not parts[1]:
                    continue
                member.name = parts[1]
            target = (destination / member.name).resolve()
            if destination.resolve() not in target.parents or member.issym() or member.islnk():
                raise RuntimeError('Unexpected archive member: ' + member.name)
            source.extract(member, destination)
    marker.write_text(pin['sha256'])
    return destination


def fetch_oidn(output):
    """The OIDN SDK unpacked in output/oidn (include, lib, bin, doc)."""
    archive = output / 'oidn.zip'
    pin = OIDN_WINDOWS
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != pin['sha256']:
        temporary = archive.with_suffix('.download')
        urllib.request.urlretrieve(pin['url'], temporary)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != pin['sha256']:
            raise RuntimeError('oidn: archive checksum mismatch')
        temporary.replace(archive)
    destination = output / 'oidn'
    marker = destination / '.remix-pin'
    if marker.exists() and marker.read_text() == pin['sha256']:
        return destination
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    with zipfile.ZipFile(archive) as source:
        for member in source.infolist():
            parts = member.filename.split('/', 1)
            if len(parts) != 2 or not parts[1] or member.is_dir():
                continue
            target = (destination / parts[1]).resolve()
            if destination.resolve() not in target.parents:
                raise RuntimeError('Unexpected archive member: ' + member.filename)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(source.read(member))
    marker.write_text(pin['sha256'])
    return destination


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--ndk', type=Path)
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    pin = json.loads((root / 'scripts/common/dependencies.lock.json').read_text())['dependencies']['sdl3']
    source = fetch(output, 'sdl3', pin)
    build = output / 'build-sdl3'
    options = []
    if args.ndk:
        options += ['-DCMAKE_TOOLCHAIN_FILE=' + str(args.ndk.resolve() / 'build/cmake/android.toolchain.cmake'),
                    '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29']
    elif platform.system() == 'Windows':
        # Name the LLVM tools explicitly so CMake never falls back to an MSYS2 ld/ar on PATH.
        tools = {name: shutil.which(name) for name in ('lld-link', 'llvm-lib')}
        missing = [name for name, path in tools.items() if not path]
        if missing:
            raise RuntimeError('Missing LLVM tools on PATH: ' + ', '.join(missing))
        options += ['-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl',
                    '-DCMAKE_LINKER=' + tools['lld-link'], '-DCMAKE_AR=' + tools['llvm-lib']]
    else:
        options += ['-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++']
        if platform.system() == 'Darwin':
            options += ['-DCMAKE_OSX_ARCHITECTURES=arm64',
                        '-DCMAKE_OSX_DEPLOYMENT_TARGET=' + os.environ.get('MACOSX_DEPLOYMENT_TARGET', '13.0')]
    subprocess.run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(output / 'prefix'),
        '-DCMAKE_INSTALL_LIBDIR=lib', '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF',
        '-DSDL_TEST_LIBRARY=OFF', '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF', *options], check=True)
    subprocess.run(['cmake', '--build', str(build), '--parallel', str(args.jobs)], check=True)
    subprocess.run(['cmake', '--install', str(build)], check=True)
    if not args.ndk and platform.system() == 'Windows':
        fetch_oidn(output)

if __name__ == '__main__':
    main()
