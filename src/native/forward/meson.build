oidn_deps = []
oidn_args = []
if get_option('oidn_root') != ''
  oidn_root = get_option('oidn_root')
  oidn_deps += declare_dependency(
    compile_args : ['-I' + oidn_root / 'include'],
    dependencies : cpp.find_library('OpenImageDenoise', dirs : [oidn_root / 'lib']))
  oidn_args += '-DREMIX_OIDN=1'
endif
lightmap_denoise_library = static_library('remix_lightmap_denoise', 'lightmap/denoise.cpp', 'lightmap/denoise_chart.cpp', 'lightmap/denoise_graph.cpp', 'lightmap/denoise_filter.cpp', 'lightmap/denoise_device.cpp',
  dependencies : oidn_deps + [threads_dep], include_directories : [remix_public_include, native_include],
  cpp_args : oidn_args, gnu_symbol_visibility : 'hidden', pic : true)

forward_library = static_library('remix_forward',
  'bake/reference_capture.cpp',
  'bake/visibility_report.cpp',
  'bake/probe_depth.cpp',
  'bake/controller_probe_depth.cpp',
  'fragment_statistics.cpp', 'lighting.cpp', 'resources.cpp', 'renderer.cpp', 'targets.cpp', 'pipelines.cpp', 'scene.cpp', 'shadows/sun_cache.cpp', 'geometry_uploads.cpp', 'rendering.cpp', 'exposure.cpp', 'sky.cpp', 'image_dump.cpp',
  'viewmodel/target.cpp', 'viewmodel/lighting.cpp', 'viewmodel/scene.cpp', 'viewmodel/rendering.cpp',
  'reflections.cpp', 'ray/capture.cpp', 'ray/live_scene.cpp', 'ray/reflections.cpp',
  'ray_scene.cpp',
  'material.cpp', 'bake/data.cpp', 'bake/light_sampling.cpp', 'bake/policy.cpp', 'bake/probe_visibility.cpp', 'bake/probe_visibility_gpu.cpp', 'bake/probe_visibility_plan.cpp', 'bake/cache.cpp', 'bake/transport.cpp', 'bake/indexed_visibility.cpp', 'bake/pipelines.cpp', 'bake/atlas.cpp',
  'bake/cache_persistence.cpp', 'water/snapshot.cpp', 'water/data.cpp', 'water/pass.cpp',
  'shadows/plan.cpp', 'shadows/maps.cpp', 'mixed/data.cpp', 'mixed/refine.cpp', 'mixed/direct_work.cpp', 'mixed/ownership.cpp', 'mixed/directional_links.cpp', 'mixed/unified_masks.cpp', 'mixed/publication.cpp', 'mixed/overflow.cpp', 'mixed/masks.cpp', 'mixed/mask_codec.cpp', 'mixed/mask_palette.cpp', 'mixed/occlusion.cpp', 'mixed/map.cpp', 'lightmap/layout.cpp', 'lightmap/receiver_footprint.cpp', 'lightmap/receiver_sampler.cpp', 'bake/sun_antialiasing.cpp', 'lightmap/packing.cpp', 'lightmap/fit.cpp', 'lightmap/parameterization.cpp', 'lightmap/curved.cpp', 'lightmap/harmonic.cpp', 'lightmap/fan.cpp', 'lightmap/seams.cpp', 'lightmap/curved_seams.cpp', 'lightmap/seam_mesh.cpp', 'lightmap/surface_walk.cpp', 'lightmap/coverage.cpp', 'lightmap/interior_donors.cpp', 'lightmap/data.cpp', 'lightmap/geometry.cpp', 'lightmap/decal.cpp', 'bake/grid.cpp', 'bake/grid_sampling.cpp', 'bake/grid_directory.cpp', 'bake/sparse_grid.cpp', 'bake/adaptive_grid.cpp', 'bake/receiver_support.cpp', 'bake/refinement_builder.cpp', 'bake/controller.cpp', 'bake/controller_refinement.cpp',
  'mixed/mask_pages.cpp', 'mixed/directional_field.cpp', 'mixed/directional_fields.cpp', 'mixed/directional_capture.cpp', 'mixed/directional_coverage.cpp', 'bake/receiver_report.cpp', 'bake/receiver_projection.cpp', 'bake/receiver_tiles.cpp', 'bake/sun_depth_layout.cpp', 'bake/sun_depth_cache.cpp', 'bake/sun_depth.cpp', 'bake/controller_sun_depth.cpp', 'bake/grouped_direct_report.cpp', 'bake/fingerprint.cpp', 'bake/relocation.cpp', 'bake/styles.cpp', 'bake/sparse_sh.cpp', 'bake/style_work.cpp', 'bake/styled_direct.cpp', 'bake/controller_styles.cpp', 'bake/controller_storage.cpp', 'bake/controller_directionals.cpp', 'bake/controller_probe_visibility.cpp',
  'probes/data.cpp', 'probes/composition.cpp', 'probes/image_data.cpp', 'probes/image_asset.cpp', 'probes/capture_work.cpp', 'probes/receivers.cpp', 'probes/receiver_mask.cpp', 'probes/authoring.cpp', 'probes/capture_dump.cpp', 'probes/placement.cpp', 'probes/selection.cpp', 'probes/evaluation.cpp', 'probes/regions.cpp', 'probes/sites.cpp',
  link_with : [scene_library, raster_library, pipeline_library, lightmap_denoise_library, postfx_library],
  dependencies : vulkan_dep, include_directories : [remix_public_include, native_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)
