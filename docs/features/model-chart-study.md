# Offline model chart study

`remix-model-chart-study` checks external chart data against the native CPU
lightmap pipeline. It does not add an unwrapper dependency or enable model baking.
No Vulkan device or GPU bake is created.

Build and run (Windows build-directory example):

```powershell
meson compile -C _work/build/forwardplus remix-model-chart-study
_work/build/forwardplus/tests/forward/remix-model-chart-study.exe chart-data.txt
_work/build/forwardplus/tests/forward/remix-model-chart-study.exe chart-data.txt --baseline
```

Without a file, the registered test uses a synthetic triangle. `--baseline`
regenerates consecutive model charts on the dataset's original index stream.
Both paths use 0.248 metre manual spacing, zero padding, a 1024 maximum page size,
four maximum pages and a 4,194,304 texel limit. The native fitter may coarsen or
compact pages. These are study settings, not renderer defaults.

## Input format

UTF-8 whitespace-separated text, at most 16 MiB:

```text
MCHART1
vertex_count index_count chart_count
position_x position_y position_z normal_x normal_y normal_z material_u material_v
... one vertex row per input vertex ...
... index_count zero-based vertex indices ...
face_count
original_face_index chart_u0 chart_v0 chart_u1 chart_v1 chart_u2 chart_v2
... one face row per face in this chart ...
... repeat face_count and face rows for every chart ...
```

Positions are metres. Indices address the original vertices; each original
triangle is three consecutive indices. Face rows preserve the original corner
order. Every face must belong to exactly one chart. Input is limited to 98,304
vertices and indices (32,768 triangles), with no trailing data.

The reader preserves vertex attributes and builds a private chart-contiguous
index stream. Per-chart UV bounds are normalized to a 2-by-2 source chart before
the native world-metric parameterizer runs. Curved charts may be reparameterized
by that pipeline. Original and final UV mappings are validated independently;
complete surface sampling and mesh lightmap-coordinate coverage are required.

The JSON result reports chart rectangles, bake-point count, page allocation,
effective spacing and mapping coverage. An input/coverage failure exits nonzero.

## Limits of the evidence

This tool studies one draw with no BSP partition, authored chart gaps, materials,
lights, controllable groups or live draw bindings. Composite datasets can contain
several model geometries, but do not exercise their separate draw/material
identities. A translated export
also changes world-grid phase. Passing it does not qualify whole-map atlas fit,
lighting accuracy, bake time, memory, frame cost or an unwrapper dependency.
The default synthetic test does not exercise a captured external dataset.

## Whole-scene eligibility inventory

`REMIX_DEBUG_BAKE_MODEL_PREFLIGHT` names a fresh JSON output for the initial
preparation worker. It streams the same uncharted opaque static triangle
eligibility used by the chart generator, independently of the chart cap. The
report includes accepted counts, full admitted triangle area, source-vertex
references per draw and shared raw-geometry groups. Area is not clipped visible
area, and raw grouping does not establish reuse across different transforms or
eligible face subsets. Lighting cannot be shared merely because geometry matches.

This diagnostic stops preparation after writing the inventory: replay exits
nonzero with `Model eligibility preflight complete`. That is intentional and
must not be reported as a successful lighting bake. No charts, atlas fit or GPU
bake are performed. Without the environment variable, it does not run.

Limits are 32,768 source draws, four million accepted triangles and two million
source vertices in one draw. Vertex sets stream one draw at a time. Cancellation,
file overwrite protection and geometry/count invariants are tested.
