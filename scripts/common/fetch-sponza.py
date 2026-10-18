#!/usr/bin/env python3
"""Downloads Crytek Sponza (Morgan McGuire's OBJ edition, CC BY 3.0) for remix-sponza-sample.

The archive is pinned by SHA-256 and extracted to _work/assets/sponza by default:
sponza.obj, sponza.mtl, textures/ and copyright.txt (the attribution).
"""
import argparse
import hashlib
import pathlib
import shutil
import sys
import tempfile
import urllib.request
import zipfile

URL = "https://casual-effects.com/g3d/data10/common/model/crytek_sponza/sponza.zip"
SHA256 = "da005cbee0be2df2abc8513f3ceb61bcb6f69aac112babcd9c00169a27c2770c"
ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--output", type=pathlib.Path, default=ROOT / "_work" / "assets" / "sponza")
  args = parser.parse_args()
  output = args.output.resolve()
  if (output / "sponza.obj").exists():
    print(f"Sponza already present: {output / 'sponza.obj'}")
    return 0
  with tempfile.TemporaryDirectory() as temporary:
    archive = pathlib.Path(temporary) / "sponza.zip"
    print(f"Downloading {URL}")
    with urllib.request.urlopen(URL) as response, open(archive, "wb") as file:
      shutil.copyfileobj(response, file)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SHA256:
      print(f"SHA-256 mismatch: expected {SHA256}, got {digest}", file=sys.stderr)
      return 1
    staging = pathlib.Path(temporary) / "sponza"
    with zipfile.ZipFile(archive) as zip:
      for member in zip.infolist():
        target = (staging / member.filename).resolve()
        if not target.is_relative_to(staging.resolve()):
          print(f"Unsafe archive path: {member.filename}", file=sys.stderr)
          return 1
      zip.extractall(staging)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
      shutil.rmtree(output)
    shutil.move(str(staging), output)
  print(f"Sponza extracted to {output}")
  return 0


if __name__ == "__main__":
  sys.exit(main())
