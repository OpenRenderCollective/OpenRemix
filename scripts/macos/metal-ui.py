#!/usr/bin/env python3
"""Check that the Metal copies of the developer panel's tabs (src/native/metal/ui) were ported
from the current upstream tabs (src/native/ui). --update records the current tabs after porting."""
import argparse
import hashlib
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
record = root / 'src/native/metal/ui/upstream.txt'
tabs = ['diagnostics.cpp', 'lighting.cpp', 'post_processing.cpp', 'rendering.cpp', 'textures.cpp']


def blob(path):
    # The file's git blob ID, so `git diff <recorded> <current>` shows what changed.
    data = path.read_bytes()
    return hashlib.sha1(b'blob %d\0' % len(data) + data).hexdigest()


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--update', action='store_true')
args = parser.parse_args()
current = {tab: blob(root / 'src/native/ui' / tab) for tab in tabs}
if args.update:
    record.write_text('# Upstream src/native/ui tabs the Metal copies match (scripts/macos/metal-ui.py --update).\n' +
                      ''.join(f'{current[tab]} {tab}\n' for tab in tabs))
    sys.exit(0)
recorded = dict(reversed(line.split()) for line in record.read_text().splitlines() if not line.startswith('#'))
stale = [tab for tab in tabs if recorded.get(tab) != current[tab]]
for tab in stale:
    print(f'src/native/ui/{tab} changed since src/native/metal/ui/{tab} was ported; port the change:\n'
          f'  git diff {recorded.get(tab, "<unrecorded>")} {current[tab]}', file=sys.stderr)
if stale:
    print('then run scripts/macos/metal-ui.py --update', file=sys.stderr)
sys.exit(1 if stale else 0)
