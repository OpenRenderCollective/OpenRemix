#!/usr/bin/env python3
"""Build the pinned shader tools (glslang, SPIRV-Tools; with --metal the SPIRV-Cross fork; with --slang slangc) once per machine.

The renderer's SPIR-V and Metal libraries are generated at build time (generate.py, metal.py), so every
build needs the same tools: pinned revisions give every machine the same output. They are built for the
build machine with the environment's compiler (a cross file's compilers are the target's, never used
here) into a cache that every build directory shares. --output records the tools for generate.py --tools.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
LOCK = json.loads(Path(__file__).with_name('toolchain.lock.json').read_text())
EXE = '.exe' if os.name == 'nt' else ''
# Raise to rebuild every cached toolset after changing how one is built.
LAYOUT = 1


def fetch(entry, target):
    subprocess.run(['git', 'init', '-q', str(target)], check=True)
    subprocess.run(['git', '-C', str(target), 'fetch', '-q', '--depth', '1', entry['repository'], entry['revision']],
                   check=True)
    subprocess.run(['git', '-C', str(target), 'checkout', '-q', 'FETCH_HEAD'], check=True)


def build(source, directory, options, targets):
    """Configure and build only the named targets (installing would build every library and test)."""
    subprocess.run(['cmake', '-S', str(source), '-B', str(directory), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                    *options], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(['cmake', '--build', str(directory), '--target', *targets], check=True, stdout=subprocess.DEVNULL)


def executable(directory, name):
    matches = [path for path in directory.rglob(name + EXE) if path.is_file()]
    if len(matches) != 1:
        raise SystemExit(f'{directory}: expected one {name}{EXE}, found {len(matches)}')
    return matches[0]


def khronos(work, bin_dir):
    """glslangValidator, spirv-opt and spirv-val."""
    fetch(LOCK['glslang'], work / 'glslang')
    # The optimizer is spirv-opt's: glslang needs no SPIRV-Tools of its own.
    build(work / 'glslang', work / 'glslang-build', ['-DENABLE_OPT=OFF', '-DENABLE_HLSL=OFF', '-DGLSLANG_TESTS=OFF',
                                                     '-DGLSLANG_ENABLE_INSTALL=OFF'], ['glslang-standalone'])
    fetch(LOCK['spirv-tools'], work / 'spirv-tools')
    fetch(LOCK['spirv-headers'], work / 'spirv-tools/external/spirv-headers')
    build(work / 'spirv-tools', work / 'spirv-tools-build', ['-DSPIRV_SKIP_TESTS=ON', '-DSPIRV_WERROR=OFF'],
          ['spirv-opt', 'spirv-val'])
    # glslang-standalone builds 'glslang'; glslangValidator is the name every script calls.
    shutil.copy2(executable(work / 'glslang-build', 'glslang'), bin_dir / ('glslangValidator' + EXE))
    for name in ('spirv-opt', 'spirv-val'):
        shutil.copy2(executable(work / 'spirv-tools-build', name), bin_dir / (name + EXE))


def spirv_cross(work, bin_dir):
    """The SPIRV-Cross fork that translates SPIR-V for the Metal backend."""
    fetch(LOCK['spirv-cross-msl'], work / 'spirv-cross')
    build(work / 'spirv-cross', work / 'spirv-cross-build', ['-DSPIRV_CROSS_ENABLE_TESTS=OFF',
                                                             '-DSPIRV_CROSS_SHARED=OFF'], ['spirv-cross'])
    # metal.py checks the revision in --version (git describe), so keep it with the source's history.
    shutil.copy2(executable(work / 'spirv-cross-build', 'spirv-cross'), bin_dir / ('spirv-cross' + EXE))


def slang(work, bin_dir):
    """slangc and the libraries beside it, from the pinned release archive (NUMOS sky shaders)."""
    machine = {'amd64': 'x86_64', 'x86_64': 'x86_64', 'arm64': 'aarch64', 'aarch64': 'aarch64'}[platform.machine().lower()]
    system = {'win32': 'windows', 'linux': 'linux', 'darwin': 'macos'}[sys.platform]
    entry = LOCK['slang'].get(f'{system}-{machine}')
    if entry is None:
        raise SystemExit(f'No pinned slangc release for {system}-{machine}')
    work.mkdir(parents=True)
    archive = work / 'slang.tar.gz'
    with urllib.request.urlopen(entry['url']) as response:
        archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != entry['sha256']:
        raise SystemExit('slangc release archive checksum mismatch')
    with tarfile.open(archive) as tar:
        tar.extractall(work / 'slang', filter='data')
    slangc = executable(work / 'slang', 'slangc')
    # slangc loads its compiler libraries (slang, slang-glslang, ...) from its own directory.
    libraries = slangc.parent if system == 'windows' else slangc.parent.parent / 'lib'
    for path in [*slangc.parent.iterdir(), *(libraries.iterdir() if libraries.is_dir() else [])]:
        if path.is_file():
            shutil.copy2(path, bin_dir / path.name)


def remove_tree(path, ignore_errors=False):
    """Removes a directory, including the read-only files Git checkouts hold on Windows."""
    for parent, _, files in os.walk(path):
        for name in files:
            try:
                os.chmod(os.path.join(parent, name), stat.S_IWRITE | stat.S_IREAD)
            except OSError:
                pass
    shutil.rmtree(path, ignore_errors=ignore_errors)


def toolset(cache, name, entries, make):
    """The cached toolset's bin directory, built on first use. Concurrent builds (several build
    directories) each build in a scratch directory and the first to finish publishes it."""
    key = hashlib.sha256(json.dumps([LAYOUT, [LOCK[e] for e in entries]], sort_keys=True).encode()).hexdigest()[:16]
    final = cache / f'{name}-{key}'
    if (final / 'complete').exists():
        return final / 'bin'
    cache.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix=name + '-', dir=cache))
    try:
        (scratch / 'bin').mkdir()
        print(f'Building pinned {name} shader tools in {final} (once per machine)', flush=True)
        make(scratch / 'work', scratch / 'bin')
        remove_tree(scratch / 'work')
        (scratch / 'complete').touch()
        try:
            scratch.rename(final)
        except OSError:
            if not (final / 'complete').exists():
                raise
    finally:
        remove_tree(scratch, ignore_errors=True)
    return final / 'bin'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='JSON record of the tools (generate.py --tools)')
    parser.add_argument('--cache', default='', help='Toolset cache (default: _work/tools/shader-toolchain)')
    parser.add_argument('--metal', action='store_true', help='Also build the SPIRV-Cross fork')
    parser.add_argument('--slang', action='store_true', help='Also fetch the pinned slangc release (NUMOS sky)')
    args = parser.parse_args()
    cache = Path(args.cache).resolve() if args.cache else ROOT / '_work/tools/shader-toolchain'
    bins = [toolset(cache, 'khronos', ['glslang', 'spirv-tools', 'spirv-headers'], khronos)]
    record = {'path': [str(bins[0])]}
    if args.metal:
        record['spirv-cross'] = str(toolset(cache, 'spirv-cross', ['spirv-cross-msl'], spirv_cross) /
                                    ('spirv-cross' + EXE))
    if args.slang:
        record['slangc'] = str(toolset(cache, 'slang', ['slang'], slang) / ('slangc' + EXE))
    args.output.write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    main()
