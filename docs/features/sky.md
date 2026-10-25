# Sky

`rtx.native.sky.enabled` (0/1, default 0; the **Sky** tab, **NUMOS sky**) draws a
physically based sky: atmosphere, volumetric clouds, weather, aerial
perspective (haze, height fog, sun shafts), the sun, the moon, stars and
lightning. It is [NUMOS](https://github.com/Kim2091/numos-sky) by Kim2091
(Sparkles), Apache-2.0, built from the `third_party/numos/numos-sky` submodule
(`-Dsky=disabled` leaves it out). The path tracer uses it fully (below); the
raster backend draws the sky, clouds and haze over its frame (Raster). The
Metal backend does not draw it yet.

## What it does to the frame

- **Sky.** Camera rays that miss the scene see NUMOS's sky and clouds; every
  other ray that misses (bounces, sky lighting, volumetrics, SHARC) samples
  NUMOS's sky baked into a 512×256 environment each frame.
- **The light.** The scene's distant light is the brightest of NUMOS's sun,
  moon and lightning flash. A flash kilometres away lights the scene as a disc
  of its size would.
- **Cloud shadows.** The sun's light on surfaces and in the volumetric medium
  passes through the clouds: NUMOS's cloud shadow map, resampled into a 4 km
  box around the camera in 8 m texels.
- **Haze.** Aerial perspective and NUMOS's height fog cover the scene before
  upscaling. The opaque world (`trace::skyOccluderInstanceMask`, instance mask
  bit 3) shadows them, and NUMOS measures the ground level from it.
- **Game sky.** `rtx.native.sky.gameSky`: `hide` (default) draws NUMOS in place
  of the game's sky, `under` keeps the game's sky behind the clouds and haze,
  `over` leaves sky pixels alone.

## Raster

With the raster backend, NUMOS's sky and clouds replace the game's sky (or
sit over or under it, `gameSky`) and its haze covers the world, before the view
model and the HUD. The frame is display-referred, so the sky's light is
exposed by `rtx.native.sky.rasterExposure` (default 1) and tone mapped.
There is no scene light: the game lights its own world. Without the path
tracer's TLAS the haze is unshadowed and NUMOS uses the configured ground
level.

## Settings

The **Sky** tab shows the common settings with ranges for each, a weather
preset menu, and every NUMOS setting under **All NUMOS settings** (the tooltip
of each is NUMOS's description). All of them are `rtx.atmosphere.*` keys,
NUMOS's own names, saved in the profile and settable with
`SetConfigVariable`.

| Setting | Default | |
|---|---|---|
| `rtx.native.sky.enabled` | 0 | The NUMOS sky. |
| `rtx.native.sky.gameSky` | hide | `hide`, `under`, `over` (above). |
| `rtx.native.sky.radianceScale` | 1 | Scales the sky's light in the scene. |
| `rtx.native.sky.ignoreGameControl` | 0 | Keep the profile's time and weather when the game sets them (API below). |
| `rtx.native.pathtrace.volumetricUpAxis` | 3 | The world's up axis: 1 Y, 2 Z, 3 Auto (from the camera). A wrong axis turns the sky on its side. |
| `rtx.atmosphere.hazeWorldScale` | 1 | The haze's world relative to the game's. Below 1 hazes small worlds (0.2 makes a block of 1 m count as 5 m of air). |
| `rtx.atmosphere.timeOfDayHours` | 12 | The time of day; `timeCycleEnable` runs it, a day in `dayLengthMinutes`. |
| `rtx.atmosphere.nightGainStops` | 0 | Brightens the night (moonlight, starlight, airglow) for a display. |

The scale of the world comes from `rtx.native.pathtrace.sceneScale` (meters per
game unit). The stars and Milky Way are NUMOS's procedural galaxy, generated
at build time (`scripts/shaders/sky.py`, which needs numpy; without it the sky
has no stars). The moon's surface is NASA's (ThirdPartyLicenses.txt).

## API

`public/include/remix/remix_sky_ext.h`: the game's live time, weather and
lightning, never saved, applied over the profile unless it sets
`ignoreGameControl`. Look the functions up by name.

| Function | |
|---|---|
| `remixapi_SetSkyTimeEXT` | Time of day, day of year and the clock's day length; null hands time back. |
| `remixapi_SetSkyWeatherPresetEXT` | Eases to a NUMOS weather preset (Clear, Fair, PartlyCloudy, Cloudy, Overcast, Haze, Fog, Storm, Drizzle, Altostratus) and sets the precipitation intensity. |
| `remixapi_SetSkyWeatherFieldEXT` | One NUMOS weather field (cloud coverage, type, density, base, wind, fog, lightning rate…) over the preset. |
| `remixapi_ClearSkyWeatherEXT` | Eases back to the profile's weather. |
| `remixapi_TriggerSkyLightningEXT` | A strike, at a position or placed automatically. |
| `remixapi_GetSkyStateEXT` | Last frame: active, the distant light's kind and direction, time of day, weather transition, cloud coverage, precipitation. |

## How it runs

`src/native/sky/`: `bridge.cpp` implements NUMOS's GPU interface over the
renderer's Vulkan device; `renderer.cpp` records NUMOS's frame after the TLAS
builds, bakes the environment and the cloud shadow box (`shaders/*.slang`,
OpenRemix's adapters), dispatches the screen clouds after the scene's depth
and composites before the upscaler. The shaders are Slang, compiled at build
time with a pinned slangc (`scripts/shaders/toolchain.py --slang`) and checked
against NUMOS's pass table; their SPIR-V translates to MSL with the pinned
SPIRV-Cross. The Sky tab's GPU time needs **GPU pass timers** (Diagnostics).
