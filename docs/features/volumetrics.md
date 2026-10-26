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

The game's fog is what `remixapi_SetFogState` last gave: exponential fog's
density is the extinction; squared-exponential and linear fog get the
exponential that leaves 5% of the light at the distance where theirs does.
The fog colour, normalized, tints what the medium scatters: how bright the
fog is comes from the lights. A game that sets no fog has no medium unless the
override is on.

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
