pathtrace_library = static_library('remix_pathtrace',
  'blue_noise.cpp', 'diagnostics.cpp',
  'light_guiding.cpp', 'sky_guiding.cpp', 'restir.cpp', 'regir.cpp', 'sparse.cpp', 'volume.cpp', 'orca.cpp', 'local_lights.cpp', 'camera_culling.cpp', 'pbr_reuse.cpp', 'ray_profile.cpp', 'sharc.cpp', 'gbuffer.cpp', 'temporal.cpp', 'blas.cpp', 'geometry.cpp', 'memory.cpp', 'renderer.cpp', 'rendering.cpp', 'tlas.cpp', 'tlas_policy.cpp', 'shader_code.cpp',
  link_with : [scene_library, raster_library, denoise_library, upscale_library],
  dependencies : [vulkan_dep, nrd_dep, xz_dep], include_directories : [remix_public_include, native_include, image_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)
