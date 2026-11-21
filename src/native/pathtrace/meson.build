# Acceleration structures for devices without ray queries, built with TinyBVH
# (third_party/tinybvh, whose headers set the warning level).
software_bvh_library = static_library('remix_software_bvh', 'software_bvh.cpp',
  include_directories : [native_include, include_directories('../../../third_party/tinybvh/include', is_system : true)],
  gnu_symbol_visibility : 'hidden', pic : true,
  # TinyBVH takes _MSC_VER to mean its SSE 4.2 code compiles; clang-cl wants the feature named.
  cpp_args : cpp.get_argument_syntax() == 'msvc' ? cpp.get_supported_arguments('-msse4.2') : [],
  override_options : ['warning_level=0', 'werror=false'])
pathtrace_library = static_library('remix_pathtrace',
  'blue_noise.cpp', 'diagnostics.cpp',
  'light_guiding.cpp', 'sky_guiding.cpp', 'restir.cpp', 'regir.cpp', 'sparse.cpp', 'volume.cpp', 'orca.cpp', 'local_lights.cpp', 'camera_culling.cpp', 'pbr_reuse.cpp', 'ray_profile.cpp', 'sharc.cpp', 'gbuffer.cpp', 'temporal.cpp', 'blas.cpp', 'geometry.cpp', 'memory.cpp', 'renderer.cpp', 'rendering.cpp', 'tlas.cpp', 'tlas_policy.cpp', 'shader_code.cpp',
  pathtrace_shaders[0], pathtrace_shaders[2],
  link_with : [scene_library, raster_library, denoise_library, upscale_library, software_bvh_library] + sky_libraries,
  dependencies : [vulkan_dep, nrd_dep, xz_dep, numos_dep], include_directories : [remix_public_include, native_include, image_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)
