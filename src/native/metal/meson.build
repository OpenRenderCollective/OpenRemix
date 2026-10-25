# The native Metal backend (gpu_backend=metal): the same libremix and exported
# API as the Vulkan build, with the Metal runtime and renderers. Shared code is
# limited to the API, scene, platform, settings/profiles and UI panel sources,
# none of which depends on a GPU API.
metal_args = shell_args + ['-fobjc-arc'] + cpp.get_supported_arguments('-Wno-nullability-completeness')
metal_sources = files(
  'denoise.mm',
  'ui/diagnostics.cpp',
  'ui/lighting.cpp',
  'ui/post_processing.cpp',
  'ui/rendering.cpp',
  'ui/textures.cpp',
  'post.mm',
  'presentation.mm',
  'raster.mm',
  'raster_overlays.mm',
  'reconstruction.mm',
  'runtime.mm',
  'shaders.mm',
  'tracer.mm',
  'timers.mm',
  'tracer_geometry.mm',
  'ui.mm',
)
metal_shared_sources = files(
  '../pathtrace/tlas_policy.cpp',
  '../runtime/profiles.cpp',
  '../runtime/settings.cpp',
  '../ui/mods.cpp',
  '../ui/menu_shortcut.cpp',
  '../ui/panel.cpp',
  '../ui/profile_actions.cpp',
  '../ui/profiles.cpp',
  '../ui/style.cpp',
)
