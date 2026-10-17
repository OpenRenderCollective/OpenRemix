#!/usr/bin/env python3
"""Bundle a Windows runtime and its configured USD/OIDN dependencies.

Creates a fresh directory suitable for a wrapper's openremix directory. SDK
locations come from the actual Meson build; no installed game is modified.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


class Bundle:
    def __init__(self, destination):
        self.destination = destination
        self.files = {}
        self.sources = {}

    def copy(self, source, relative=None):
        source = source.resolve(strict=True)
        relative = Path(relative or source.name)
        key = relative.as_posix()
        if key.casefold() in self.sources:
            raise ValueError(f"Conflicting runtime file: {relative}")
        self.sources[key.casefold()] = (source, relative)

    def tree(self, source, relative):
        source = source.resolve(strict=True)
        for item in sorted(source.rglob("*")):
            if item.is_file():
                self.copy(item, Path(relative) / item.relative_to(source))

    def write(self):
        # Resolve and validate every input before creating the destination.
        self.destination.mkdir(parents=True)
        for source, relative in self.sources.values():
            target = self.destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            self.files[relative.as_posix()] = {
                "source": str(source),
                "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
            }


def package(build, sdl, output):
    root = Path(__file__).resolve().parents[2]
    options = {
        entry["name"]: entry["value"]
        for entry in json.loads((build / "meson-info/intro-buildoptions.json").read_text())
    }
    if output.exists():
        raise ValueError("Use a fresh output directory")
    bundle = Bundle(output)
    bundle.copy(build / "src/native/remix.dll")
    bundle.copy(sdl)
    for source in ("LICENSE", "ThirdPartyLicenses.txt"):
        bundle.copy(root / source, "licenses/" + source)
    notices = {
        "third_party/imgui/LICENSE.txt": "ImGui.txt",
        "third_party/image/LICENSES.txt": "ImageDecoders.txt",
        "third_party/nrd/LICENSE.txt": "NRD.txt",
        "third_party/nrd/External/MathLib/LICENSE.txt": "MathLib.txt",
        "third_party/nrd/External/ShaderMake/LICENSE.txt": "ShaderMake.txt",
        "third_party/nrd/External/sse2neon/LICENSE": "sse2neon.txt",
        "third_party/sharc/License.md": "SHARC.md",
        "src/native/forward/shaders/SBOX-LICENSE.txt": "sbox-SSR.txt",
        "src/native/pathtrace/shaders/LICENSE-eon.txt": "EON.txt",
        "third_party/fsr31/LICENSE.txt": "FSR31.txt",
        "third_party/xz-embedded/COPYING": "XZ-Embedded.txt",
    }
    for source, name in notices.items():
        bundle.copy(root / source, "licenses/" + name)
    bundle.tree(root / "include/vulkan/LICENSES", "licenses/Vulkan-Headers")
    bundle.copy(root / "include/vulkan/LICENSE.md", "licenses/Vulkan-Headers/NOTICE.md")
    # The pinned SDL fallback may use SDL3_shared.dll instead of SDL3.dll.
    # Keep its actual import name and require the corresponding source notice.
    sdl_notices = sorted((root / "subprojects").glob("SDL3-*/LICENSE.txt"))
    if len(sdl_notices) != 1:
        raise ValueError("Expected one pinned SDL source license")
    bundle.copy(sdl_notices[0], "licenses/SDL3.txt")

    oidn = Path(options["oidn_root"]) if options.get("oidn_root") else None
    if options.get("usd"):
        usd = Path(options["usd_root"])
        libraries = sorted((usd / "lib").glob("*.dll"))
        if not libraries:
            raise FileNotFoundError(f"No USD runtime DLLs in {usd}")
        for source in libraries:
            bundle.copy(source)
        bundle.tree(usd / "lib/usd", "usd")
        # Plugin paths are relative to their own plugInfo.json files.
        bundle.tree(usd / "plugin/usd", "plugin/usd")
        bundle.copy(root / "src/native/scene/OpenUSD-LICENSE.txt", "licenses/OpenUSD.txt")
        tbb = Path(options["usd_tbb_root"])
        for source in sorted((tbb / "bin").glob("*.dll")):
            # OIDN supplies the newer oneTBB runtime needed by its plugins.
            # Use one provider per DLL; record the choice in the manifest.
            if not oidn or not (oidn / "bin" / source.name).exists():
                bundle.copy(source)
        if not oidn:
            bundle.copy(tbb.parent / "sources/onetbb/LICENSE.txt", "licenses/oneTBB.txt")
            bundle.copy(tbb.parent / "sources/onetbb/third-party-programs.txt",
                        "licenses/oneTBB-third-party-programs.txt")
    if oidn:
        for name in ("OpenImageDenoise.dll", "OpenImageDenoise_core.dll",
                     "OpenImageDenoise_device_cpu.dll", "tbb12.dll"):
            if not (oidn / "bin" / name).is_file():
                raise FileNotFoundError(oidn / "bin" / name)
        # Device backends are loaded dynamically, so a PE import walk alone
        # would omit them. Include the SDK's full DLL set, without its tools.
        for source in sorted((oidn / "bin").glob("*.dll")):
            bundle.copy(source)
        for source in sorted((oidn / "doc").glob("*.txt")):
            bundle.copy(source, "licenses/OIDN/" + source.name)

    dlss = json.loads((build / "src/native/upscale/dlss-runtime.json").read_text())
    if dlss["enabled"]:
        expected = {Path(name).name: digest for digest, name in
                    (line.split("  ", 1) for line in
                     (root / "third_party/dlss/sources.sha256").read_text().splitlines())}
        for name in ("nvngx_dlss.dll", "nvngx_dlssd.dll", "nvngx_dlssg.dll"):
            source = build / "src/native/upscale" / name
            if hashlib.sha256(source.read_bytes()).hexdigest() != expected[name]:
                raise ValueError("DLSS release library hash mismatch: " + name)
            bundle.copy(source, Path("dlss") / dlss["version"] / name)
        for name in ("LICENSE.txt", "README.remix.md", "upstream.json", "sources.sha256"):
            bundle.copy(root / "third_party/dlss" / name,
                        Path("dlss") / dlss["version"] / name)
    bundle.write()
    manifest = {
        "build": str(build),
        "usd": bool(options.get("usd")),
        "oidn": str(oidn) if oidn else None,
        "tbb_provider": "OIDN" if oidn else "USD" if options.get("usd") else None,
        "files": bundle.files,
    }
    (output / "runtime-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--sdl", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = package(args.build.resolve(strict=True), args.sdl.resolve(strict=True), args.output.resolve())
    print(f"Packaged {len(manifest['files'])} runtime files in {args.output}")


if __name__ == "__main__":
    main()
