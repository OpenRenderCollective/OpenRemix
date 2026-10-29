# Subsurface scattering

Light that enters a surface and leaves it nearby gives skin, wax, marble, jade
and leaves their soft, coloured shadow edges and their glow where they are
thin. The path tracer models it cheaply with Burley's normalized diffusion:
direct light reaching a visible subsurface point is gathered at a point the
profile picks around it, on the same surface, by one short probe ray. Light
sampling, shadows and reflections are unchanged, and indirect light stays plain
diffuse. **Subsurface scattering** in the path tracer's settings
(`rtx.native.pathtrace.subsurface`, default on) turns it off everywhere. The
raster backend and Metal ignore it.

## Parameters

The controls are Blender's Principled ones. `remixapi_MaterialSubsurfaceEXT`
on an opaque `MaterialInfo`; in USD, attributes on the material's shader
outside the MDL inputs (AperturePBR has none for them, so RTX Remix ignores
them).

| Parameter | USD | Default | Range |
|---|---|---|---|
| weight: the share of diffuse light that scatters (0: none) | `openremix:subsurface:weight` | 0 | 0–1 |
| radius: how far red, green and blue travel (mean free path), relative | `openremix:subsurface:radius` (`float3`) | (1, 0.2, 0.1) | ≥ 0 |
| scale: the radius's size in metres | `openremix:subsurface:scale` | 0.05 | > 0 |
| anisotropy: forward (+) or backward (−) scattering | `openremix:subsurface:anisotropy` | 0 | −0.9–0.9 |

Metres become scene units by `rtx.native.pathtrace.sceneScale`. Forward
scattering carries light further: the radius is divided by 1 − anisotropy.
RTX Remix's `remixapi_MaterialInfoOpaqueSubsurfaceEXT` with
`subsurfaceDiffusionProfile` is read as weight 1, its radius, and its radius
scale in millimetres.

## How it is shaded

- For the share `weight` of the visible point's direct-light samples, a radius
  is drawn from one channel's profile (Golubev 2018, the CDF inverted in closed
  form) at a random angle in the surface's tangent plane.
- A probe ray from above that point, along the surface's normal, collects every
  crossing of the same surface within 16 times the largest radius and keeps one
  at random. Light is gathered there; its diffuse part is weighted per channel
  by the profile over the density it was drawn with (so red, with the largest
  radius, bleeds furthest), and the reflection keeps the original point's
  shading normal.
- A crossing behind the nearest is the far side of a thin object: light
  reaching that side glows through (ears, leaves, candles), and the reflection
  is dropped for that sample. A single-sided plane has no far side.
- No crossing: the point shades as it is. Probing only along the normal darkens
  silhouettes slightly, as in the cheap disk sampling of other renderers.
- Cost: one short ray query per subsurface sample, nothing else: no extra light
  samples, shadow rays, passes or buffers. The raster G-buffer does not hold the
  parameters, so a subsurface pixel's primary ray is traced again for them (as a
  thin film's is).

## Recipes

| Look | Weight | Radius | Scale (m) | Anisotropy |
|---|---|---|---|---|
| Skin | 1 | (1, 0.2, 0.1) | 0.01 | 0 |
| Wax / candle | 1 | (1, 0.7, 0.4) | 0.03 | 0.3 |
| Marble | 0.8 | (1, 0.95, 0.9) | 0.02 | 0 |
| Jade | 1 | (0.4, 1, 0.6) | 0.05 | 0.5 |
| Leaf | 0.6 | (0.6, 1, 0.3) | 0.005 | 0.7 |
