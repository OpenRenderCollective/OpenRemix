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
    # Replace only the model module. The actual material pass stays unchanged,
    # and default implementation files are absent rather than incidentally
    # supplying declarations to the custom model.
    source = tree / "shaders/forward.frag"
    source.write_bytes((root / "src/native/forward/shaders/forward.frag").read_bytes())
    model_files = ("standard-shading.glsl", "direct-lighting.glsl", "standard-local-lighting.glsl")
    for name in model_files:
        (tree / "shaders" / name).unlink()
    shutil.copyfile(root / "tests/forward/shaders/custom-model.glsl",
                    tree / "shaders/shading-model.glsl")
    variants = {
        "world": [],
        "world-guides": ["REFLECTION_GUIDES"],
        "viewmodel": ["VIEWMODEL_PASS", "REFLECTION_GUIDES"],
        "live-diffuse": ["LIVE_GI", "REFLECTION_GUIDES"],
        "live-viewmodel": ["LIVE_GI", "VIEWMODEL_PASS"],
        "cascade-diffuse": ["CASCADE_GI", "REFLECTION_GUIDES"],
        "cascade-viewmodel": ["CASCADE_GI", "VIEWMODEL_PASS"],
        "composed-model": ["LIVE_GI", "OPENREMIX_BAKED_DIRECT_LIVE_INDIRECT"],
        "composed-model-viewmodel": ["LIVE_GI", "OPENREMIX_BAKED_DIRECT_LIVE_INDIRECT", "VIEWMODEL_PASS"],
    }
    for name, defines in variants.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        *["-D" + define for define in defines], str(source),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
    # Restore the actual standard material, then swap only its environment
    # source. World resolve, viewmodels and water keep their real composition.
    for name in (*model_files, "shading-model.glsl"):
        shutil.copyfile(root / "src/native/forward/shaders" / name, tree / "shaders" / name)
    source.write_text((root / "src/native/forward/shaders/forward.frag").read_text(), encoding="utf-8")
    shutil.copyfile(root / "tests/forward/shaders/custom-environment.glsl",
                    tree / "shaders/reflection-environment.glsl")
    (tree / "shaders/probes.glsl").unlink()
    (tree / "shaders/probe-tiles.glsl").unlink()
    for name in ("shaders/environment.glsl", "shaders/environment-sampling.glsl",
                 "viewmodel/shaders/environment.glsl", "water/shaders/environment.glsl"):
        (tree / name).unlink()
    variants = {
        "environment-viewmodel": (source, ["VIEWMODEL_PASS", "REFLECTION_GUIDES"]),
        "environment-live-viewmodel": (source, ["LIVE_GI", "VIEWMODEL_PASS"]),
        "environment-cascade-viewmodel": (source, ["CASCADE_GI", "VIEWMODEL_PASS"]),
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
    for name in ("probes.glsl", "probe-tiles.glsl"):
        shutil.copyfile(root / "src/native/forward/shaders" / name, tree / "shaders" / name)
    for name in ("shaders/environment.glsl", "shaders/environment-sampling.glsl",
                 "viewmodel/shaders/environment.glsl", "water/shaders/environment.glsl"):
        shutil.copyfile(root / "src/native/forward" / name, tree / name)
    # Replace only the screen/ray transport source. Keep the actual resolve,
    # environment fallback and material Fresnel, with no bilateral source code.
    shutil.copyfile(root / "tests/forward/shaders/custom-reflection-transport.glsl",
                    tree / "shaders/reflection-transport.glsl")
    binary = tree / "reflection-transport-source.spv"
    subprocess.run([compiler, "-V", "--target-env", "vulkan1.1", "-DSSR_FILTER",
                    str(tree / "shaders/reflections.comp"), "-o", str(binary)], env=env, check=True)
    subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
    shutil.copyfile(root / "src/native/forward/shaders/reflection-transport.glsl",
                    tree / "shaders/reflection-transport.glsl")
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
    # A non-Source-2 source can replace the scalar guide convention without
    # changing the material pass or reflection resolve. Replace the default
    # adapter entirely, not an extra declaration after it.
    shutil.copyfile(root / "tests/forward/shaders/custom-guide-reference.glsl",
                    tree / "shaders/reflection-guide-reference.glsl")
    shutil.copyfile(root / "tests/forward/shaders/custom-environment.glsl",
                    tree / "shaders/reflection-environment.glsl")
    for name, (stage, defines) in {
        "guide-domain-world": (source, ["REFLECTION_GUIDES"]),
        "guide-domain-viewmodel": (source, ["VIEWMODEL_PASS", "REFLECTION_GUIDES"]),
        "guide-domain-resolve": (tree / "shaders/reflections.comp", ["SSR_FILTER"]),
    }.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        *["-D" + define for define in defines], str(stage),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
    for name in ("reflection-guide-reference.glsl", "reflection-environment.glsl"):
        shutil.copyfile(root / "src/native/forward/shaders" / name, tree / "shaders" / name)
    shutil.copyfile(root / "src/native/forward/shaders/receiver-lighting.glsl",
                    tree / "shaders/receiver-lighting.glsl")
    # Compile the real composed source through standard material passes. This
    # validates the shader contract only; runtime resource selection is separate.
    for name, defines in {
        "composed-world": [],
        "composed-metal": ["ZERO_DIFFUSE_MATERIAL"],
        "composed-viewmodel": ["VIEWMODEL_PASS"],
        "composed-water": ["WATER_SURFACE", "RAY_SCENE_SET=3"],
        "composed-water-ray": ["WATER_SURFACE", "WATER_RAY", "RAY_SCENE_SET=3"],
    }.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.2",
                        "-DLIVE_GI", "-DOPENREMIX_BAKED_DIRECT_LIVE_INDIRECT", "-DREFLECTION_GUIDES",
                        *["-D" + define for define in defines], str(source),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.2", str(binary)], env=env, check=True)
    # Real cascade gathering composes with baked receivers without a ray-pass
    # define. Host selection remains guarded until these pipelines are qualified.
    for name, defines in {
        "composed-cascade-world": [],
        "composed-cascade-prepassed": ["PREPASSED_WORLD"],
        "composed-cascade-viewmodel": ["VIEWMODEL_PASS"],
        "composed-cascade-early-viewmodel": ["VIEWMODEL_PASS", "EARLY_OPAQUE"],
        "composed-cascade-metal": ["ZERO_DIFFUSE_MATERIAL"],
        "composed-cascade-water": ["WATER_SURFACE"],
        "composed-cascade-ray-water": ["WATER_SURFACE", "WATER_RAY"],
    }.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.2", "-DCASCADE_GI",
                        "-DOPENREMIX_BAKED_DIRECT_LIVE_INDIRECT", "-DREFLECTION_GUIDES",
                        *["-D" + define for define in defines], str(source),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.2", str(binary)], env=env, check=True)
    # Replace incoming transport while retaining baked direct storage, contexts,
    # mixed-shadow attribution and the real standard material. No ray include is
    # available, so accidental dependencies cannot pass on unused declarations.
    source.write_text((root / "src/native/forward/shaders/forward.frag").read_text().replace(
        '#include "gi.glsl"', ''), encoding="utf-8")
    (tree / "shaders/gi.glsl").unlink()
    (tree / "shaders/incoming-cascades.glsl").unlink()
    (tree / "cascades/shaders/receiver-frame.glsl").unlink()
    (tree / "shaders/incoming-frame.glsl").write_text("", encoding="utf-8")
    shutil.copyfile(root / "tests/forward/shaders/custom-incoming-source.glsl",
                    tree / "shaders/incoming-lighting.glsl")
    for name, defines in {
        "incoming-source-world": [],
        "incoming-source-viewmodel": ["VIEWMODEL_PASS"],
        "incoming-source-metal": ["ZERO_DIFFUSE_MATERIAL"],
        "incoming-source-water": ["WATER_SURFACE"],
    }.items():
        for ray_pass in (False, True):
            binary = tree / (name + ("-ray-pass" if ray_pass else "-non-ray") + ".spv")
            subprocess.run([compiler, "-V", "--target-env", "vulkan1.2",
                            *(["-DLIVE_GI"] if ray_pass else []),
                            "-DOPENREMIX_BAKED_DIRECT_LIVE_INDIRECT", "-DREFLECTION_GUIDES",
                            *["-D" + define for define in defines], str(source),
                            "-o", str(binary)], env=env, check=True)
            subprocess.run([validator, "--target-env", "vulkan1.2", str(binary)], env=env, check=True)
    for name, defines in {
        "incoming-source-cascade-world": [],
        "incoming-source-cascade-viewmodel": ["VIEWMODEL_PASS"],
    }.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.2", "-DCASCADE_GI",
                        *["-D" + define for define in defines], str(source),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.2", str(binary)], env=env, check=True)
    source.write_text((root / "src/native/forward/shaders/forward.frag").read_text(), encoding="utf-8")
    for name in ("gi.glsl", "incoming-lighting.glsl", "incoming-cascades.glsl", "incoming-frame.glsl"):
        shutil.copyfile(root / "src/native/forward/shaders" / name, tree / "shaders" / name)
    shutil.copyfile(root / "src/native/forward/cascades/shaders/receiver-frame.glsl",
                    tree / "cascades/shaders/receiver-frame.glsl")
    # The alternate local source must not rely on default Source 2 transport
    # declarations being incidentally imported by the pass/material response.
    # It supplies queries only: the real standard material must acquire its
    # response adapter from the shared material module, not this fixture.
    (tree / "shaders/light-sample.glsl").unlink()
    (tree / "shaders/local-light-storage.glsl").unlink()
    (tree / "shaders/occlusion.glsl").unlink()
    (tree / "shaders/local-shadows.glsl").unlink()
    shutil.copyfile(root / "tests/forward/shaders/custom-local-source.glsl",
                    tree / "shaders/local-lighting.glsl")
    for name, defines in {
        "local-source-world": [],
        "local-source-viewmodel": ["VIEWMODEL_PASS"],
        "local-source-live-world": ["LIVE_GI"],
        "local-source-cascade-world": ["CASCADE_GI"],
        "local-source-cascade-viewmodel": ["CASCADE_GI", "VIEWMODEL_PASS"],
        "local-source-water": ["WATER_SURFACE"],
    }.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        *["-D" + define for define in defines], str(source),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
    shutil.copyfile(root / "src/native/forward/shaders/light-sample.glsl",
                    tree / "shaders/light-sample.glsl")
    for name in ("occlusion.glsl", "local-shadows.glsl", "local-light-storage.glsl"):
        shutil.copyfile(root / "src/native/forward/shaders" / name, tree / "shaders" / name)
    shutil.copyfile(root / "src/native/forward/shaders/local-lighting.glsl",
                    tree / "shaders/local-lighting.glsl")
    # A replacement sunlight source needs only values and receiver queries,
    # not the default baked visibility/CSM implementation.
    (tree / "shaders/sun.glsl").unlink()
    (tree / "shaders/sun-shadows.glsl").unlink()
    shutil.copyfile(root / "tests/forward/shaders/custom-sun-source.glsl",
                    tree / "shaders/sun-lighting.glsl")
    for name, defines in {
        "sun-source-world": [],
        "sun-source-viewmodel": ["VIEWMODEL_PASS"],
        "sun-source-live-world": ["LIVE_GI"],
        "sun-source-cascade-world": ["CASCADE_GI"],
        "sun-source-cascade-viewmodel": ["CASCADE_GI", "VIEWMODEL_PASS"],
        "sun-source-water": ["WATER_SURFACE"],
        "sun-source-water-ray": ["WATER_SURFACE", "WATER_RAY"],
        "sun-source-composed-water": ["WATER_SURFACE", "LIVE_GI", "RAY_SCENE_SET=3",
                                       "OPENREMIX_BAKED_DIRECT_LIVE_INDIRECT"],
    }.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        *["-D" + define for define in defines], str(source),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
    # Replace the whole lighting set and remove default baked storage. Pass
    # coverage metadata must remain usable without a lightmap/probe backend.
    for fixture, target in (("custom-diffuse-source.glsl", "receiver-lighting.glsl"),
                            ("custom-local-source.glsl", "local-lighting.glsl"),
                            ("custom-sun-source.glsl", "sun-lighting.glsl"),
                            ("custom-environment.glsl", "reflection-environment.glsl")):
        shutil.copyfile(root / "tests/forward/shaders" / fixture, tree / "shaders" / target)
    for name in ("baked-storage.glsl", "irradiance.glsl", "style-lighting.glsl", "lightmap.glsl",
                 "occlusion.glsl", "local-shadows.glsl", "light-sample.glsl", "local-light-storage.glsl"):
        (tree / "shaders" / name).unlink()
    for name, (stage, defines) in {
        "unified-world": (source, []),
        "unified-guides": (source, ["REFLECTION_GUIDES"]),
        "unified-metal": (source, ["ZERO_DIFFUSE_MATERIAL", "REFLECTION_GUIDES"]),
        "unified-viewmodel": (source, ["VIEWMODEL_PASS", "REFLECTION_GUIDES"]),
        "unified-live": (source, ["LIVE_GI"]),
        "unified-live-viewmodel": (source, ["LIVE_GI", "VIEWMODEL_PASS"]),
        "unified-water": (source, ["WATER_SURFACE"]),
        "unified-ray-water": (source, ["WATER_SURFACE", "WATER_RAY"]),
        "unified-live-water": (source, ["WATER_SURFACE", "LIVE_GI", "RAY_SCENE_SET=3"]),
        "unified-resolve": (tree / "shaders/reflections.comp", ["SSR_FILTER"]),
    }.items():
        binary = tree / (name + ".spv")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        *["-D" + define for define in defines], str(stage),
                        "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
print("Custom models and environment/transport/diffuse/local/sun sources compose through actual material passes")
