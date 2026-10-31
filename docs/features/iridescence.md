# Iridescence

A thin-film coating colours a material's reflection the way a soap
bubble, an oil slick or a beetle's shell is coloured: light reflected off the
film's top and bottom interferes, and which wavelengths survive depends on the
film's thickness, its refractive index and the viewing angle. The path tracer
evaluates the film spectrally (Belcour and Barla 2017: the Airy sum of the
film's interreflections, integrated against the CIE colour matching functions
in closed form), so colours sweep smoothly across a surface and thick films
fade to pastel instead of aliasing into stripes. The raster backend ignores
the film. **Iridescence** in the path tracer's settings
(`rtx.native.pathtrace.iridescence`, default on) turns every film off.

RTX Remix's thin film is one Airy layer at three fixed wavelengths with the
film's index fixed at 1.5. OpenRemix reads the same inputs, so a Remix mod's
iridescence shows as authored, and adds controls beyond them.

## Parameters

| Parameter | API (`remixapi_MaterialIridescenceEXT`) | USD on the AperturePBR shader | Default |
|---|---|---|---|
| on | `strength > 0` | `inputs:enable_thin_film` | off |
| thickness, nm (0–2000) | `thickness` | `inputs:thin_film_thickness_constant` | 200 |
| thickness from albedo alpha (× 1500 nm; the material becomes opaque) | opaque info's `alphaIsThinFilmThickness` | `inputs:thin_film_thickness_from_albedo_alpha` | off |
| strength (0–1): how much of the reflection is the film's | `strength` | `openremix:iridescence:strength` | 1 |
| film index (1–3): higher is more saturated | `filmRefractiveIndex` | `openremix:iridescence:filmIor` | 1.5 |
| thickness map: red lerps min to max | `thicknessTexture`, `thicknessMin`, `thicknessMax` | `openremix:iridescence:thicknessTexture`, `…:thicknessMin`, `…:thicknessMax` | none, 100, 800 |
| flow amplitude, nm: animated swirls added to the thickness | `flowAmplitude` | `openremix:iridescence:flowAmplitude` | 0 (still) |
| flow scale: the swirls' size in scene units, object space | `flowScale` | `openremix:iridescence:flowScale` | 1 |
| flow speed, per second | `flowSpeed` | `openremix:iridescence:flowSpeed` | 0.1 |
| structural colour (0–1): the film's hue tints diffuse reflection too | `structural` | `openremix:iridescence:structural` | 0 |

The `openremix:iridescence:*` attributes are float (asset for the texture)
attributes on the shader prim, outside its `inputs:`, so MDL compilation in the
RTX Remix Toolkit ignores them. Without the extension, the opaque info's
`thinFilmThickness` gives a film of strength 1 and index 1.5, as RTX Remix
does. Remix Studio (the Godot toolkit) edits all of them in its material
inspector.

## How it is shaded

- The film replaces the specular Fresnel term, blended by its strength, at the
  angle to the microfacet normal: in the BRDF, its lobe selection, the PSR
  mirror's reflectance and DLSS-RR's specular albedo guide. The diffuse lobe
  keeps what the film does not reflect, so it takes the complementary hue.
- Structural colour multiplies the surface's albedo by the film's hue (at full
  brightness) along the view, so rough surfaces, whose reflection blurs the
  bands away, keep their colour.
- The film sits over the surface's own reflectance: a dielectric reads it as an
  index, a metal (by metallic) through its base colour.
- Flow is domain-warped gradient noise in the instance's object space, so the
  swirls stay on a moving object.
- The raster G-buffer does not hold the film: a filmed pixel's primary ray is
  traced again for its material (as a decal's is).

## Glass and water

A translucent material (`remixapi_MaterialInfoTranslucentEXT`, USD
`AperturePBR_Translucent`) takes the film too; RTX Remix's translucent
material has none, so in USD the same `inputs:enable_thin_film` and
`inputs:thin_film_thickness_constant` are read there by OpenRemix alone.
Thickness from albedo alpha and structural colour do not apply.

- The surface becomes a smooth dielectric (roughness 0.04) under the film.
- Its reflection counts whole, whatever the sheet's coverage, and what passes
  through is less what the film reflects: the light behind takes the
  complementary colour, as through a soap bubble. Against an evenly lit
  background the two add back to white, so a film shows best where the
  surface reflects something brighter or darker than what lies behind it.
- A body (water, no thin wall) uses the film's reflectance in place of its
  Fresnel term, at the body's refractive index; a thin wall at index 1.5.
- The denoised front sheet keeps the split: coverage weighs only its diffuse
  light. Deeper sheets that reuse the front one's light reflect as unfilmed
  sheets do.

## Recipes

| Look | Thickness | Film index | Strength | Flow | Structural | Surface |
|---|---|---|---|---|---|---|
| Soap bubble | 300 | 1.33 | 1 | 250 nm, scale 0.5, speed 0.15 | 0 | glass-like: metallic 0, roughness 0, dark albedo |
| Oil slick | 450 | 1.5 | 1 | 400 nm, scale 2, speed 0.05 | 0.3 | black, roughness 0.1 |
| Beetle shell | 550 | 1.8 | 1 | none | 0.8 | dark green, metallic 0.3, roughness 0.25 |
| Alien chrome | 700 (or a map, 200–1200) | 2.4 | 1 | 600 nm, scale 1, speed 0.3 | 0.5 | metallic 1, roughness 0.05 |
| Opal | map, 150–900 | 1.45 | 0.7 | 150 nm, scale 0.2, speed 0.02 | 1 | white, roughness 0.4 |
| Soap-film glass | 350 | 1.33 | 1 | 300 nm, scale 0.4, speed 0.2 | — | translucent, thin-walled, transmittance near white |
| Petrol on water | 500 | 1.5 | 0.8 | 500 nm, scale 3, speed 0.05 | — | translucent body (water) |
