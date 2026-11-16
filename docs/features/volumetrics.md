# Volumetrics

`rtx.native.pathtrace.volumetrics` (0/1, default 1; the Lighting tab's
**Volumetrics** section, **Volumetric fog**) adds a participating medium to the path tracer: fog
and haze that the sun, the sky and every local light shine through. It costs
nothing until there is a medium.

## The medium

| Setting (`rtx.native.pathtrace.`) | Default | |
|---|---|---|
| `volumetricOverride` | 0 | 0: the game's fog. 1: the medium described below. |
| `volumetricDistance` | 80 | Override: how far one sees through it, in meters (5% of the light is left there). |
| `volumetricColor.r/g/b` | 1 1 1 | Override: what the medium scatters. |
| `volumetricScattering` | 1 | Scale of the scattered light. |
| `volumetricHeightFalloff` | 0 | Meters over which the density falls to 37% above the base height. 0: uniform. |
| `volumetricBaseHeight` | 0 | Height (meters) below which the density is whole. |
| `volumetricUpAxis` | 3 | The world's up axis, for the height falloff and the NUMOS sky: 0 X, 1 Y, 2 Z, 3 Auto (Y or Z, from the camera: game cameras do not roll). |
| `volumetricMaxDistance` | 250 | The medium fills at most this many meters of a camera ray, the sky's included. |
| `volumetricFogDensityScale` | 1 | Scales the game's exponential fog density for the traced medium (raster fog takes it as given). |
| `volumetricRaster` | 0 | Raster fog (below). |
| `volumetricRasterBrightness` | 1 | Raster fog: the fog colour's radiance. |
| `volumetricRasterLights` | 16 | Raster fog: how many lights glow (0-64). |
| `volumetricRasterShadowSize` | 64 | Raster fog: texels a side of a glowing light's shadow cube face (16-256). |
| `volumetricRasterSteps` | 12 | Raster fog: samples of each glowing light along a camera ray (1-64). |
| `volumetricRasterSunSize` | 512 | Raster fog: texels a side of the sun's shadow map (64-2048). |
| `volumetricRasterSunExtent` | 64 | Raster fog: meters the sun's and the sky's maps reach to each side of the camera. |
| `volumetricRasterSunSteps` | 16 | Raster fog: samples of the sun and the sky along a camera ray (1-64). |
| `volumetricRasterSoftLights` | 48 | Raster fog: lights after the first `volumetricRasterLights` that glow with a coarse cube (0-128; 128 in all). |
| `volumetricRasterSoftSize` | 16 | Raster fog: texels a side of a coarse cube's face (4-256). |
| `volumetricRasterSoftSteps` | 4 | Raster fog: samples of a coarse-cube light along a camera ray (1-64). |
| `volumetricRasterSkySize` | 128 | Raster fog: texels a side of each of the sky's five maps (16-1024). |
| `volumetricRasterGlowLights`, `volumetricRasterGlowEmissive`, `volumetricRasterGlowSun`, `volumetricRasterGlowSky` | 1 | Raster fog: whether the game's lights, emissive surfaces, the sun and the sky glow. |
| `volumetricRasterClearSky` | 0 | Raster fog: no glow over the sky either, as the game's fog leaves it clear. |

The game's fog is what `remixapi_SetFogState` last gave: exponential fog's
density is the extinction; squared-exponential and linear fog get the
exponential that leaves 5% of the light at the distance where theirs does.
The fog colour, normalized, tints what the medium scatters: how bright the
fog is comes from the lights. A game that sets no fog has no medium unless the
override is on.

## Raster fog

`volumetricRaster` (0/1, default 0; **Raster fog**) draws the medium as a
raster game draws its fog instead of tracing it: each pixel is blended towards
the fog colour by its view depth, up to the first surface that is not a
blended sheet; the sky stays clear. The game's mode is kept as it is (linear
from its start to its end, `remixapi_FogInfo::scale` being 1 / (end - start)
as in D3D; exponential and squared exponential by its density); the override
is exponential fog leaving 5% at `volumetricDistance`. The fog colour is
unlit: `volumetricRasterBrightness` (1) is its radiance.

Lights, the sun and the sky glow in it, shadowed, with no history to settle
or trail:

1. Each frame the `volumetricRasterLights` (16) lights that send the most into
   view (intensity over squared distance, among those whose reach comes within
   `volumetricMaxDistance` in front of the camera) each get a shadow cube:
   `volumetricRasterShadowSize` (64) texels a side a face, the distance from
   the light to the first opaque surface, traced anew by the volume update
   pass (6 x 64^2 rays a light). Blended sheets let the light through. Sphere
   lights compete with emissive surfaces, which are summed into clusters, one
   per cell of a 2 m world grid: their area-weighted centre, their summed
   radiance times area, and their mean normal (a flat panel glows by the
   cosine, surfaces facing all ways evenly). A panel's cube starts just in
   front of it. The next `volumetricRasterSoftLights` (48) get coarse cubes of
   `volumetricRasterSoftSize` (16) texels a face, sampled
   `volumetricRasterSoftSteps` (4) times a camera ray; 128 lights at most.
2. Each camera ray that crosses a light's reach before its first surface takes
   `volumetricRasterSteps` (12) samples of it, spaced evenly in the angle the
   ray subtends at the light, so they gather where the glow is brightest. Each
   compares its distance with the four nearest texels of the cube.
3. The sun: a map of `volumetricRasterSunSize`^2 (512^2) rays down the sun's
   direction, `volumetricRasterSunExtent` (64) meters to each side of the
   camera, its texels fixed in the world so shadow edges hold still as the
   camera moves. Camera rays take `volumetricRasterSunSteps` (16) samples up
   to where the fog leaves 5% of the light, closer together near the camera;
   past the map's edge a ray keeps the last shadow it saw.
4. The sky: five equal parts of it above the horizon (a cap about the zenith
   and the band's four quarters), each a map of `volumetricRasterSkySize`^2
   (128^2) rays along the part's mean direction, as far across as the sun's,
   and its mean radiance (an 8 x 8 grid of the environment, the same every
   frame). The sun's samples take them too: under a roof the sky's light is
   gone, at a window the parts it faces come in.

The samples are offset per pixel by a fixed noise pattern, the same every
frame. A light's glow is its single scattering towards the camera within its
reach (its range, `localRange`, at most `volumetricMaxDistance`), dimmed by
the raster fog in front of it. Its scattering is the traced medium's
(`volumetricFogDensityScale` included) times `volumetricScattering`;
`volumetricLightScale`, `volumetricEmissiveScale`, `volumetricSunScale`,
`volumetricSkyScale` and each light's own volumetric radiance scale apply.
Lights past both tiers do not glow.

## How it runs

`pathtrace/volume.cpp`, `shaders/volume.glsl`.

The light the medium scatters is held in world space, in `volumetricLevels`
(4) nested grids of `volumetricCells`^3 (48) cells around the camera. The
first has cells of `volumetricCellSize` (0.25) meters and each next one cells
twice as large: 12, 24, 48 and 96 meters across by default (14 MiB).
Past the last grid the medium holds the unshadowed sun and the environment's
mean. Cells are
addressed by their world coordinate, so turning the camera changes nothing and
moving it only replaces the cells that enter a grid (they start from the
coarser grid's value).

1. **Update**: every `volumetricUpdateInterval` (8) frames a cell takes one
   sample at a random point inside it: the sun, two directions of the
   environment and the local lights, each with one visibility ray. With up to
   64 local lights every one is evaluated there; with more, 32 candidates
   drawn from the light tree's cell pools. The largest of them is traced by
   itself and one other stands for the rest (two rays). The sun and the local lights keep the mean of
   the cell's last `volumetricHistory` (8) samples. The environment's share is
   kept apart, over sixteen times as many samples, and averaged at each update
   with one neighbouring cell the sample point sees: where the sky enters
   through an opening most directions miss it, and a short average of it left
   single cells glowing.
2. **March**: when the transparency passes compose the frame, each camera ray
   takes `volumetricSteps` (12) jittered samples of the grids up to the first
   surface that is not a blended sheet, and the medium's light and
   transmittance are laid over everything composed there.

## Quality and response

The Lighting tab's **Volumetric quality** and **Volumetric response** presets
set these together; **Volumetric detail** has each on its own.

| Quality | `volumetricCellSize` | `volumetricCells` | `volumetricLevels` | `volumetricSteps` |
|---|---|---|---|---|
| Low | 0.5 | 32 | 4 | 8 |
| Medium | 0.35 | 40 | 4 | 10 |
| High (default) | 0.25 | 48 | 4 | 12 |
| Ultra | 0.2 | 64 | 4 | 20 |

| Response | `volumetricUpdateInterval` | `volumetricHistory` |
|---|---|---|
| Smooth | 8 | 16 |
| Balanced (default) | 8 | 8 |
| Fast | 4 | 6 |
| Immediate | 2 | 4 |

A smaller cell sharpens the shadows in the medium, more cells and levels
carry that further from the camera, and more steps smooth a camera ray's
gradients. A shorter update interval traces more rays each frame (the cells
of every level over the interval, up to six rays each); a shorter history follows
changes sooner and flickers more.

## Limits

- Shadows in the medium are as soft as its cells: 0.25 m near the camera,
  coarser further out. Light shafts through small openings blur.
- A change of lighting takes `volumetricUpdateInterval` x `volumetricHistory`
  frames to settle. A change of what the sky reaches (a door opened to the
  outside) takes sixteen times as long.
- Scattering is isotropic and single: no glow around the sun, no light
  scattered twice, and surfaces are not lit by the medium.
- The medium is seen by camera rays only, in front of everything: reflections
  and what lies behind glass show none of their own.
- Off with accumulation and debug views.
