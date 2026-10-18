#!/usr/bin/env python3
"""Build the arm64 shell against the standard macOS Vulkan SDK, or with --metal its native Metal backend."""
import argparse
import os
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vulkan-sdk', type=Path, help='Vulkan SDK macOS directory (Vulkan builds)')
parser.add_argument('--metal', action='store_true', help='Build the native Metal backend (no Vulkan SDK)')
parser.add_argument('--deps', type=Path, default=root / '_work/deps/macos')
parser.add_argument('--build', type=Path, help='Default: _work/build/macos (Vulkan) or _work/build/macos-metal')
parser.add_argument('--jobs', type=int, default=8)
args = parser.parse_args()
if not args.metal and not args.vulkan_sdk:
    parser.error('Vulkan builds require --vulkan-sdk (or use --metal)')
args.build = args.build or root / ('_work/build/macos-metal' if args.metal else '_work/build/macos')
env = dict(os.environ, CC='clang', CXX='clang++', OBJCXX='clang++', MACOSX_DEPLOYMENT_TARGET='13.0')
if not args.metal:
    sdk = args.vulkan_sdk.resolve()
    env.update(VULKAN_SDK=str(sdk), PKG_CONFIG_PATH=str(sdk / 'lib/pkgconfig'))
command = ['meson', 'setup', str(args.build), str(root), '-Dbuildtype=release', '-Dwerror=true',
           '-Ddeps_root=' + str(args.deps.resolve() / 'prefix'),
           '-Dgpu_backend=' + ('metal' if args.metal else 'vulkan')]
if (args.build / 'meson-private/coredata.dat').exists(): command.append('--reconfigure')
subprocess.run(command, env=env, check=True)
subprocess.run(['meson', 'compile', '-C', str(args.build), '-j', str(args.jobs)], env=env, check=True)
subprocess.run(['meson', 'test', '-C', str(args.build), '--print-errorlogs'], env=env, check=True)
