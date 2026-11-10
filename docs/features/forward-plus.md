# Forward+ renderer

OpenRemix defaults to path tracing. Select Forward+ in the Rendering tab or set
`rtx.native.backend = forwardplus`. Source 2 is the default technique preset
within Forward+. The SourcePT adapter also accepts
`OPENREMIX_PROXY_BACKEND=forwardplus`.

Start with the [fresh-clone build and deployment guide](forward-plus-setup.md).
The [roadmap](forward-plus-roadmap.md) tracks unresolved visual and performance
work, and the [handoff](forward-plus-handoff.md) explains how to reproduce results.

## Lighting and reflections

- Baked lightmaps provide static direct and indirect surface lighting. Probe
  volumes provide baked irradiance for moving receivers. Live lights remain
  available while a map bake is preparing.
- Mixed lights combine baked static visibility with dynamic caster shadows.
  Styled lights retain their own baked contribution so a style can change at
  runtime. Indexed visibility supports more than four overlapping lights,
  subject to the map bake's memory and planning budgets.
- Parallax cubemaps provide the default specular reflections. Authored volumes
  take precedence within their influence and automatic volumes fill the remaining
  space. SSR is opt-in. Viewmodels use world lighting with their own overlay and
  shadow treatment.
- Vulkan ray queries are required for hardware bake transport. Forward+ can
  still render with its runtime lighting when that bake capability is absent.

The renderer owns the bake and shading techniques. Game adapters provide scene
geometry, materials, light definitions and placement hints. SourcePT supplies
the inch-to-metre scale for Source maps. Read the
[lighting architecture](../architecture/forward-lighting.md) for provider
ownership and the [shading API](../architecture/shading-api.md) for the
material-facing contract.

## Configuration and diagnostics

The [controls catalog](forward-plus-controls.md) separates saved renderer
settings, private experiments and diagnostic switches. Avoid using a diagnostic
override when comparing default presets.

The profile directory accepts `probes.request` to write `probes.json` and
`timings.request` to write a pass timing report. Compare results after the
bake and reflection captures finish. Use an isolated cache directory when
testing a cold bake. Keep cold, disk-cache and live frame measurements separate.

The [implementation history](https://github.com/OpenRenderCollective/OpenRemix/blob/91fe80ca08f6fa916453f668c59e332f6946f9f2/docs/features/forward-plus.md)
preserves the earlier transport notes, ablation results and map studies.
