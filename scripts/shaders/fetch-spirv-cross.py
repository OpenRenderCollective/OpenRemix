#!/usr/bin/env python3
"""Build the pinned SPIRV-Cross fork that translates SPIR-V for the Metal backend."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True, help='Fresh build directory')
parser.add_argument('--cmake', default='cmake')
args = parser.parse_args()
target = args.output.resolve()
if target.exists():
    parser.error('Use a fresh output directory')
lock = json.loads(Path(__file__).with_name('toolchain.lock.json').read_text())['spirv-cross-msl']
source = target / 'source'
subprocess.run(['git', 'init', '-q', str(source)], check=True)
subprocess.run(['git', '-C', str(source), 'fetch', '-q', '--depth', '1', lock['repository'], lock['revision']],
               check=True)
subprocess.run(['git', '-C', str(source), 'checkout', '-q', 'FETCH_HEAD'], check=True)
build = target / 'build'
generator = ['-G', 'Ninja'] if shutil.which('ninja') else []
subprocess.run([args.cmake, '-S', str(source), '-B', str(build), *generator, '-DCMAKE_BUILD_TYPE=Release',
                '-DSPIRV_CROSS_ENABLE_TESTS=OFF', '-DSPIRV_CROSS_SHARED=OFF'], check=True)
subprocess.run([args.cmake, '--build', str(build), '--target', 'spirv-cross', '--parallel'], check=True)
# --version names the commit (git describe), which the generator checks.
print(f'REMIX_SPIRV_CROSS={build / "spirv-cross"}')
