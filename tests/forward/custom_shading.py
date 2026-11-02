"""Compile an alternate shading model against the real pass/source interfaces."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
tools = json.loads(Path(sys.argv[1]).read_text())["path"]
env = dict(os.environ, PATH=os.pathsep.join(tools + [os.environ.get("PATH", "")]))
compiler = shutil.which("glslangValidator", path=env["PATH"])
validator = shutil.which("spirv-val", path=env["PATH"])
assert compiler and validator, "Pinned shader tools unavailable"
output = Path(sys.argv[2])
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="custom-model-", dir=output) as temporary:
    tree = Path(temporary) / "forward"
    for source in (root / "src/native/forward").rglob("*.glsl"):
        destination = tree / source.relative_to(root / "src/native/forward")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
    fragment = (root / "src/native/forward/shaders/forward.frag").read_text()
    # Replace the standard model, rather than appending a model that could
    # accidentally depend on declarations provided by the standard one.
    fragment = fragment.replace('#include "direct-lighting.glsl"', '')
    assert fragment.count('#include "standard-shading.glsl"') == 1
    fragment = fragment.replace('#include "standard-shading.glsl"', '#include "custom-model.glsl"')
    assert fragment.count('shadeStandardSurface(evaluated, receiver)') == 1
    fragment = fragment.replace('shadeStandardSurface(evaluated, receiver)', 'shadeExampleDiffuse(evaluated, receiver)')
    source = tree / "shaders/forward.frag"
    source.write_text(fragment, encoding="utf-8")
    shutil.copyfile(root / "tests/forward/shaders/custom-model.glsl", source.parent / "custom-model.glsl")
    variants = {
        "world": [],
        "world-guides": ["REFLECTION_GUIDES"],
        "viewmodel": ["VIEWMODEL_PASS", "REFLECTION_GUIDES"],
        "live-diffuse": ["LIVE_GI", "REFLECTION_GUIDES"],
        "live-viewmodel": ["LIVE_GI", "VIEWMODEL_PASS"],
    }
    for name, defines in variants.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        *["-D" + define for define in defines], str(source),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
    # Restore the actual standard material, then swap only its environment
    # source. World resolve, viewmodels and water keep their real composition.
    source.write_text((root / "src/native/forward/shaders/forward.frag").read_text(), encoding="utf-8")
    shutil.copyfile(root / "tests/forward/shaders/custom-environment.glsl",
                    tree / "shaders/reflection-environment.glsl")
    variants = {
        "environment-viewmodel": (source, ["VIEWMODEL_PASS", "REFLECTION_GUIDES"]),
        "environment-live-viewmodel": (source, ["LIVE_GI", "VIEWMODEL_PASS"]),
        "environment-water": (source, ["WATER_SURFACE"]),
        "environment-water-ray": (source, ["WATER_SURFACE", "WATER_RAY"]),
        "environment-world-resolve": (tree / "shaders/reflections.comp", ["SSR_FILTER"]),
    }
    shutil.copyfile(root / "src/native/forward/shaders/reflections.comp", tree / "shaders/reflections.comp")
    for name, (stage, defines) in variants.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        *["-D" + define for define in defines], str(stage),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
    shutil.copyfile(root / "src/native/forward/shaders/reflection-environment.glsl",
                    tree / "shaders/reflection-environment.glsl")
    # Replace the entire diffuse source/context, with no chart-shaped fields.
    # Real standard materials must consume the interface, not its representation.
    shutil.copyfile(root / "tests/forward/shaders/custom-diffuse-source.glsl",
                    tree / "shaders/receiver-lighting.glsl")
    for name, defines in {
        "diffuse-source-world": [],
        "diffuse-source-metal": ["ZERO_DIFFUSE_MATERIAL", "REFLECTION_GUIDES"],
        "diffuse-source-viewmodel": ["VIEWMODEL_PASS"],
        "diffuse-source-live-viewmodel": ["VIEWMODEL_PASS", "LIVE_GI"],
        "diffuse-source-water": ["WATER_SURFACE"],
        "diffuse-source-live-water": ["WATER_SURFACE", "LIVE_GI"],
    }.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        *["-D" + define for define in defines], str(source),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
    shutil.copyfile(root / "src/native/forward/shaders/receiver-lighting.glsl",
                    tree / "shaders/receiver-lighting.glsl")
    shutil.copyfile(root / "tests/forward/shaders/custom-local-source.glsl",
                    tree / "shaders/local-lighting.glsl")
    for name, defines in {
        "local-source-world": [],
        "local-source-viewmodel": ["VIEWMODEL_PASS"],
        "local-source-live-world": ["LIVE_GI"],
        "local-source-water": ["WATER_SURFACE"],
    }.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        *["-D" + define for define in defines], str(source),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
print("Custom models and environment/diffuse/local sources compose through actual material passes")
