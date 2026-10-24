#!/usr/bin/env python3
"""Compare REMIX_DEBUG_BAKE_RECEIVERS reports from the same frozen scene."""
import argparse
import json
from pathlib import Path
from probe_receivers import compare_receivers


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    parser.add_argument('--max-details', type=int, default=12)
    parser.add_argument('--output', type=Path, help='Fresh JSON output; otherwise print to stdout')
    args = parser.parse_args()
    try:
        result = compare_receivers(json.loads(args.before.read_text(encoding='utf-8')),
                                   json.loads(args.after.read_text(encoding='utf-8')), args.max_details)
        text = json.dumps(result, indent=2, allow_nan=False) + '\n'
        if args.output:
            with args.output.open('x', encoding='utf-8') as output:
                output.write(text)
        else:
            print(text, end='')
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
