# Ray-tracing GPU optimizations

The Rendering tab's **GPU optimizations** group holds path-tracing options that
are off by default and do not change what is rendered. Two of them use optional
Vulkan extensions and have no effect without them.

## Opacity micromaps

`rtx.native.pathtrace.opacityMicromaps` (0/1) and
`rtx.native.pathtrace.opacityMicromapLevel` (1-8, default 4) attach
`VK_EXT_opacity_micromap` data to alpha-tested geometry. Traversal settles
micro-triangles that are always opaque or always cut out; only the rest reach
the shader's alpha test (`acceptCandidate`).

- **Proof, not approximation.** `scene::OpacityCache` already proves whole
  triangles opaque from the CPU texture copy, the alpha combiner, vertex alpha,
  tint and the compare function, with filtering, wrapping and rounding margins.
  Micromaps apply the same interval proof to micro-triangles, and also prove
  "always fails" from per-region alpha maxima (a min/max pyramid of mip 0).
  Anything unproved is stored as unknown, so traversal results are unchanged.
- **Eligible surfaces** are those the opaque-triangle split already handles:
  double-sided (or one-sided with hardware face culling), not blended, no
  transmission, compare LESS/LEQUAL/GREATER/GEQUAL. Decals are excluded because
  visibility rays skip them before the alpha test.
- **Subdivision** is chosen per triangle from its texel area (about two texels
  per micro-triangle, up to the level setting and the device limit). Proved
  regions stop subdividing early; uniform triangles use the special indices and
  store no data; identical micro-triangle arrays are shared.
- **Identity.** A micromap belongs to its immutable opacity partition. Mesh view
  keys include its hash and the BLAS input content includes its data, so the
  geometry cache, refits and texture replacement never reuse a stale one. A
  refit keeps its BLAS's micromap; changed micromap data rebuilds the BLAS.
- **Order.** `scene::microTriangleIndex` is the specification's hierarchical
  ("bird curve") order. `tools/vulkan-probes` mode `micromap` measures the
  driver's order and 4-state encoding at levels 1-5 and requires the baker's
  order to match (`-Dvulkan_probes=true -Dgpu_tests=true`).

Baking runs on the CPU when a surface's partition is first proved, so enabling
micromaps (or a new alpha-tested mesh) costs one proof per surface.

## Tests

- `remix-micromap-test`: hardware-measured order anchors, table nesting, and a
  brute-force check that every point of an opaque (transparent) micro-triangle
  passes (fails) the shader's alpha model.
- `remix-opacity-micromap-test` (GPU): identical images with micromaps off, on
  and off again, for ray-traced primary visibility, sun shadows and a refitted
  skinned mesh, and fewer shader alpha tests in the ray profile.
- The alpha coverage tests also run with micromaps enabled.
- `remix-shader-execution-reordering-test` (GPU): the SER pass runs and matches
  compute within rounding; `REMIX_TEST_SER_BENCHMARK=1` prints 1080p timings,
  and `REMIX_TEST_SER_MATERIALS=legacy|pbr|mixed` selects the material classes
  (legacy and PBR are the only classes with different shading code, so a mixed
  scene costing no more than the pure ones means there is no divergence for
  per-class hit shaders to remove).
  `REMIX_PATH_FIXED_SEED` makes every frame draw the same samples.


### Scene-dependent temporal preparation

Path tracing treats emissive-source eligibility as a prepared resource contract alongside perspective
and local-light eligibility. Source discovery is sticky for the enabled backend; the requested key
includes its split-lighting requirement. Candidate initialization and required pipeline compilation
run on the existing preparation workers, and activation retains the existing queue-idle/retirement
boundary. The active tracer uses its immutable prepared emissive hint, keeping combined lighting
while a split-layout replacement is pending. Temporary emitter visibility changes can still request
lazy sampler variants, but do not rebuild temporal targets. Genuine preparation failures retain the
existing diagnostics/fallback behavior; NOT_READY is not globally suppressed.
