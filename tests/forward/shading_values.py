"""Shading values must compile without texture/lighting/packed ABI bindings."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
tools = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))["path"]
env = dict(os.environ, PATH=os.pathsep.join(tools + [os.environ.get("PATH", "")]))
compiler = shutil.which("glslangValidator", path=env["PATH"])
validator = shutil.which("spirv-val", path=env["PATH"])
assert compiler and validator, "Pinned shader tools unavailable"
output = Path(sys.argv[2])
output.mkdir(parents=True, exist_ok=True)
common = """#version 450
#extension GL_GOOGLE_include_directive : require
#include "surface-values.glsl"
#include "surface-values.glsl"
#include "reflection-values.glsl"
#include "reflection-values.glsl"
#include "diffuse-values.glsl"
#include "diffuse-values.glsl"
#include "sun-values.glsl"
#include "sun-values.glsl"
"""
stages = {
    "vert": """void main() {
      Surface material = initSurface();
      vec3 reflected = resolveReflection(ReflectionSample(vec3(0), 0), material.color.rgb);
      ReceiverLighting diffuse = ReceiverLighting(vec3(.1), false, true);
      gl_Position = vec4(material.normal * material.roughness + reflected +
                         composeDiffuseLight(diffuse, material.color.rgb), material.color.a);
    }""",
    "frag": """layout(location=0) out vec4 result;
    void main() {
      Surface material = initSurface(vec3(0,1,0));
      material.color = vec4(.8,.3,.1,1);
      material.roughness = .6;
      ReflectionReceiver query = ReflectionReceiver(vec3(0), vec3(0), material.normal,
          vec3(0,0,1), material.roughness, -1, 0u, uvec2(0));
      SunLightSample sun = SunLightSample(vec3(0,0,1), vec3(1), 0, 1);
      ReflectionSample reflected = ReflectionSample(query.direction * sun.diffuseIrradiance, 1);
      ReceiverLighting diffuse = ReceiverLighting(vec3(.2), true, false);
      result = vec4(resolveReflection(reflected, material.color.rgb) + material.emission +
                    composeDiffuseLight(diffuse, material.color.rgb), material.color.a);
    }""",
    "comp": """layout(local_size_x=1) in;
    layout(set=0,binding=0,std430) buffer Output { vec4 value; };
    void main() {
      Surface material = initSurface();
      ReflectionSample reflected = ReflectionSample(material.normal, .5);
      ReceiverLighting diffuse = ReceiverLighting(vec3(1), false, false);
      value = vec4(resolveReflection(reflected, material.emission) +
                   composeDiffuseLight(diffuse, material.color.rgb), material.metallic);
    }""",
}
with tempfile.TemporaryDirectory(prefix="shading-values-", dir=output) as temporary:
    for stage, body in stages.items():
        source = Path(temporary) / ("material." + stage)
        binary = source.with_suffix(".spv")
        source.write_text(common + body, encoding="utf-8")
        subprocess.run([compiler, "-V", "--target-env", "vulkan1.1",
                        "-I" + str(root / "src/native/forward/shaders"),
                        str(source), "-o", str(binary)], env=env, check=True)
        subprocess.run([validator, "--target-env", "vulkan1.1", str(binary)], env=env, check=True)
print("Material, diffuse, sun and reflection values compile in vertex, fragment and compute without pass resources")
