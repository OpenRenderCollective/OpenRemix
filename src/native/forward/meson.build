oidn_deps = []
oidn_args = []
if get_option('oidn_root') != ''
  oidn_root = get_option('oidn_root')
  oidn_deps += declare_dependency(
    compile_args : ['-I' + oidn_root / 'include'],
    dependencies : cpp.find_library('OpenImageDenoise', dirs : [oidn_root / 'lib']))
  oidn_args += '-DREMIX_OIDN=1'
endif
lightmap_denoise_library = static_library('remix_lightmap_denoise', 'lightmap/denoise.cpp', 'lightmap/denoise_chart.cpp',
  dependencies : oidn_deps, include_directories : [remix_public_include, native_include],
  cpp_args : oidn_args, gnu_symbol_visibility : 'hidden', pic : true)

forward_library = static_library('remix_forward',
  'lighting.cpp', 'resources.cpp', 'renderer.cpp', 'targets.cpp', 'pipelines.cpp', 'scene.cpp', 'rendering.cpp', 'sky.cpp', 'image_dump.cpp',
  'reflections.cpp', 'ray/capture.cpp', 'ray/live_scene.cpp', 'ray/reflections.cpp',
  'ray_scene.cpp',
  'material.cpp', 'bake/data.cpp', 'bake/cache.cpp', 'bake/transport.cpp', 'bake/pipelines.cpp', 'bake/atlas.cpp',
  'water/snapshot.cpp', 'water/data.cpp', 'water/pass.cpp',
  'shadows/plan.cpp', 'shadows/maps.cpp', 'mixed/data.cpp', 'mixed/refine.cpp', 'mixed/occlusion.cpp', 'mixed/map.cpp', 'lightmap/layout.cpp', 'lightmap/packing.cpp', 'lightmap/fit.cpp', 'lightmap/parameterization.cpp', 'lightmap/curved.cpp', 'lightmap/harmonic.cpp', 'lightmap/seams.cpp', 'lightmap/curved_seams.cpp', 'lightmap/seam_mesh.cpp', 'lightmap/surface_walk.cpp', 'lightmap/coverage.cpp', 'lightmap/data.cpp', 'lightmap/geometry.cpp', 'bake/grid.cpp', 'bake/controller.cpp',
  'probes/data.cpp', 'probes/capture_dump.cpp', 'probes/placement.cpp', 'probes/regions.cpp', 'probes/sites.cpp',
  link_with : [scene_library, raster_library, pipeline_library, lightmap_denoise_library],
  dependencies : vulkan_dep, include_directories : [remix_public_include, native_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)
