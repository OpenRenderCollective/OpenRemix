# The native Metal backend (macOS builds): the same libremix and exported
# API as the Vulkan build, with the Metal runtime and renderers. Shared code is
# limited to the API, scene, platform, settings/profiles and UI panel sources,
# none of which depends on a GPU API.
metal_args = shell_args + ['-fobjc-arc'] + cpp.get_supported_arguments('-Wno-nullability-completeness')
metal_sources = files(
  'denoise.mm',
  'hdr_output.mm',
  'host_passes.mm',
  'particles.mm',
  'post.mm',
  'presentation.mm',
  'raster.mm',
  'raster_overlays.mm',
  'reconstruction.mm',
  'runtime.mm',
  'runtime_particles.mm',
  'screenshot.mm',
  'shaders.mm',
  'tracer.mm',
  'timers.mm',
  'tracer_geometry.mm',
  'ui.mm',
)
metal_shared_sources = files(
  '../pathtrace/tlas_policy.cpp',
  '../runtime/jxr_writer.cpp',
  '../runtime/profiles.cpp',
  '../runtime/settings.cpp',
  '../ui/diagnostics.cpp',
  '../ui/font.cpp',
  '../ui/menu_shortcut.cpp',
  '../ui/mods.cpp',
  '../ui/panel.cpp',
  '../ui/profile_actions.cpp',
  '../ui/profiles.cpp',
  '../ui/settings.cpp',
  '../ui/sky.cpp',
  '../ui/style.cpp',
  '../ui/textures.cpp',
  '../ui/widgets.cpp',
)
# The NUMOS sky: sky/renderer.cpp, the same as Vulkan builds', over the Metal
# bridge, and the embedded night sky.
if numos_enabled
  metal_sources += files('raster_sky.mm', 'sky_bridge.mm', 'sky_renderer.mm')
  metal_shared_sources += files('../sky/shader_code.cpp')
endif
