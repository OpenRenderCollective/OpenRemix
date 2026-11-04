# Subsurface scattering

Light that enters a surface and leaves it nearby gives skin, wax, marble, jade
and leaves their soft, coloured shadow edges and their glow where they are
thin. The controls are RTX Remix's, so a Remix mod's subsurface materials work
as authored. **Subsurface scattering** in the path tracer's settings
(`rtx.native.pathtrace.subsurface`, default on) turns it off everywhere. The
raster backend and Metal ignore it.

A material is one of two kinds:

- **Diffusion profile** (`subsurface_diffusion_profile`): a volume, for skin,
  flesh and eyes. Direct and indirect light are gathered where Burley's
  normalized diffusion carries them from.
- **Thin opaque** (no profile, `subsurface_measurement_distance` > 0): a thin
  layer, for leaves, paper and cloth. Light from behind passes through it.

## Material inputs

On an `AperturePBR_Opacity` shader in USD, or
`remixapi_MaterialInfoOpaqueSubsurfaceEXT` on an opaque `MaterialInfo`.

| USD input | Kind | Meaning | Default |
|---|---|---|---|
| `subsurface_diffusion_profile` | | which kind | off |
| `subsurface_radius` | profile | how far red, green and blue travel. A distance, not a tint: (1, 0.4, 0.2) reads as skin, white as wax | (0.5, 0.5, 0.5) |
| `subsurface_radius_texture` | profile | multiplies the radius, per pixel | |
| `subsurface_radius_scale` | profile | the radius's size: 1 is 0.1 mm | 1 |
| `subsurface_max_sample_radius` | profile | the farthest a sample reaches, in metres, at most 1 | 16 (so 1) |
| `subsurface_transmittance_color` | both | profile: multiplies the albedo. Thin: the colour left after the measurement distance | profile white, thin (0.5, 0.5, 0.5) |
| `subsurface_transmittance_texture` | both | replaces the colour. Profile: the surface's colour, in place of the albedo | |
| `subsurface_volumetric_anisotropy` | both | forward (+) or backward (−) scattering, −0.9 to 0.9 | 0 |
| `subsurface_measurement_distance` | thin | the layer's thickness; at or below 0.05 it is Lambert transmission, not a volume | 0 |
| `subsurface_thickness_texture` | thin | replaces the measurement distance, per pixel | |
| `subsurface_single_scattering_albedo` | thin | how much of the light in the layer scatters | (0.5, 0.5, 0.5) |
| `subsurface_single_scattering_texture` | thin | replaces the albedo above | |
| `openremix:subsurface:weight` | both | OpenRemix's own, which Remix ignores: the share of the diffuse light that scatters, 0 to 1 (`remixapi_MaterialSubsurfaceWeightEXT`) | 1 |

## Settings

These keep RTX Remix's names.

| Setting | Meaning | Default |
|---|---|---|
| `rtx.subsurface.enableDiffusionProfile` | diffusion profiles; off: plain diffuse | on |
| `rtx.subsurface.enableThinOpaque` | thin opaque surfaces; off: plain opaque | on |
| `rtx.subsurface.enableTextureMaps` | the maps above; off: their constants | on |
| `rtx.subsurface.enableTransmission` | a profile's light through thin parts | on |
| `rtx.subsurface.diffusionProfileScale` | multiplies every profile's radius | 1 |
| `rtx.subsurface.surfaceThicknessScale` | multiplies every thickness | 1 |

## Where it differs from RTX Remix

Each of these keeps a Remix mod's result where the mod is calibrated, and removes
a trap around it.

- **Scale.** Remix's radius is in world units of its `SSS_METERS_UNIT` (0.01) and
  ignores `rtx.sceneScale`, so a value only means what it says in a centimetre
  scene: too small by 100 in a metre scene. Here it is a real size through
  `rtx.native.pathtrace.sceneScale`: 22 is about 2 mm in any scene. A centimetre
  scene looks the same in both.
- **A body split across materials.** Remix rejects a sample that lands on another
  subsurface material, leaving dark seams at every boundary. Any surface with a
  profile is accepted here.
- **No transmittance texture.** Remix's diffuse lobe is replaced, so the surface
  becomes the flat transmittance colour (grey by default). Here it stays the
  albedo, multiplied by that colour. A transmittance texture that is the albedo
  texture reads exactly as the albedo, tints and vertex colours included.
- **`subsurface_max_sample_radius` 0.** Remix silently turns the profile off. Here
  it is no limit, as if unauthored.

## How it is shaded

Profile:

- For the share `weight` of the visible point's samples, a radius is drawn from
  one channel's profile (Golubev 2018, the CDF inverted in closed form) at a
  random angle in the surface's tangent plane. Each channel's profile length is
  its radius over RTXCR's scaling of the colour and anisotropy, as in Remix.
- A probe ray from above that point, along the surface's normal, collects every
  crossing of a surface with a profile and keeps one at random. Light is gathered
  there; its diffuse part is weighted per channel by the profile over the density
  it was drawn with (so red, with the largest radius, bleeds furthest), and the
  reflection keeps the original point's shading normal.
- A crossing behind the nearest is the far side of a thin object: light reaching
  that side glows through (ears, candles), and the reflection is dropped for that
  sample. A single-sided plane has no far side.
- No crossing: the point shades as it is.

Thin opaque:

- Half the samples look at the surface's far side, lit as a white diffuse surface
  there and tinted by what the layer passes; the other half are its own diffuse
  and reflection. Each is weighted for that choice, as Remix's lobes are.
- A layer thicker than 0.05 uses Remix's first-order scattering (Hanrahan 1993).
  One tint serves all of a pixel's light, so the view's cosine stands in for the
  light's: anisotropy shapes how much passes, not its direction per light.
- Thinner than that is Lambert transmission: half the diffuse each way.

Both apply to what the camera sees (and mirrors). Bounce rays hitting these
surfaces see their colour, lit as plain diffuse. The raster G-buffer does not hold
the parameters, so a subsurface pixel's primary ray is traced again for them (as a
thin film's is). A profile adds one short ray query per sample; a thin surface adds
no rays.

## Recipes

Keep the ratios if you retune: `rtx.subsurface.diffusionProfileScale` scales every
profile together. Bind the albedo as the transmittance texture, or leave it unbound.

| Look | Radius | Radius scale | Roughness |
|---|---|---|---|
| Skin | (1, 0.4, 0.2) | 22 | 0.5 |
| Interior flesh (mouth, gums) | (1, 0.4, 0.2) | 6 | 0.225 |
| Eyeball (the cornea is thin-walled translucent, not subsurface) | (1, 0.4, 0.2) | 1.5 | 1 |
| Wax / candle | (1, 0.7, 0.4) | 100 | 0.4 |
| Marble | (1, 0.95, 0.9) | 60 | 0.3 |
| Jade | (0.4, 1, 0.6) | 150 | 0.2 |

| Look | Measurement distance | Transmittance | Single scattering albedo |
|---|---|---|---|
| Leaf | 0.5 | (0.6, 1, 0.3) | (0.8, 0.9, 0.5) |
| Paper | 0.04 | white | |
