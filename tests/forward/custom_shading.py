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
    }
    for name, defines in variants.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        *["-D" + define for define in defines], str(source),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
print("Custom diffuse model compiles without the standard model in world, viewmodel and live-diffuse passes")
