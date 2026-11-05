# USD mods

Replacements in `rtx-remix/mods/*/mod.usda` load the way RTX Remix loads them: once, when the game
starts, with a progress line in the bottom left of the screen. The game draws its own assets until
every replacement is ready. From then on a replacement draws from the first frame its mesh does.

## The load

All of it runs off the Present thread (`usd/mods.cpp` `parse`):

1. **Opening USD.** A mod's layers open on several threads, each with its asset paths anchored and
   its references pointed at private copies, so nothing on disk changes.
2. **Meshes.** Every mesh composes on several threads (`ComposeScope::meshSink`), with its identical
   corners welded and identical geometry held once (`PreparedMeshes`). Geometry goes straight to
   `scene::Surface`, never through JSON, and stays in host memory as RTX Remix keeps it; the renderer
   uploads what is drawn. The USD stage is released when the load ends.
3. **Materials.** Replacements, lights and materials are read into small tables.
4. **Textures.** Only each file's mips up to 128 pixels are read (`scene::loadTexturePreview`); a
   mipped block-compressed DDS needs just its end. Textures that lights use are read whole.

The Present thread then creates materials and meshes for at most a few milliseconds a frame
(`Submission::publish`), and the whole set takes effect at once.

## Streaming

A material starts on its textures' low mips. The first time a draw resolves to it
(`Material::streamUse`), its whole files decode on a background thread and the material moves to
them, at most 32 MiB of new textures a frame so uploads spread out (`Submission::service`).

When the process nears its video memory budget (over 90% of `VK_EXT_memory_budget`), materials not
drawn for 300 frames return to their low mips, which frees their whole files once nothing else uses
them. Under budget nothing is evicted. Materials of scene instances are kept, because instance replay
does not report use.

## Reloading

A change to a mod's files, or the Mods tab's Reload, repeats the whole load. The previous set stays
active until the new one is ready. A mod that fails to parse keeps the previous set and reports why.

## Measuring

`remix-usd-mods-probe <mods directory> [meters per unit] [up axis]` (built with the USD tests) loads
a mods directory without a GPU and reports each stage's time, memory, and the slowest frame boundary.
