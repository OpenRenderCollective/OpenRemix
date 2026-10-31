#!/usr/bin/env python3
"""Build the arm64 shell with its native Metal backend."""
import argparse
import os
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--deps', type=Path, default=root / '_work/deps/macos')
parser.add_argument('--build', type=Path, default=root / '_work/build/macos')
parser.add_argument('--jobs', type=int, default=8)
args = parser.parse_args()
env = dict(os.environ, CC='clang', CXX='clang++', OBJCXX='clang++', MACOSX_DEPLOYMENT_TARGET='13.0')
command = ['meson', 'setup', str(args.build), str(root), '-Dbuildtype=release', '-Dwerror=true',
           '-Ddeps_root=' + str(args.deps.resolve() / 'prefix')]
if (args.build / 'meson-private/coredata.dat').exists(): command.append('--reconfigure')
subprocess.run(command, env=env, check=True)
subprocess.run(['meson', 'compile', '-C', str(args.build), '-j', str(args.jobs)], env=env, check=True)
subprocess.run(['meson', 'test', '-C', str(args.build), '--print-errorlogs'], env=env, check=True)
