upscale_library = static_library('remix_upscale', 'renderer.cpp', 'sdk_backend.cpp', 'ngx.cpp', 'neural_uplift.cpp',
  'fsr_frame_generation.cpp', upscale_shaders,
  link_with : [pipeline_library, postfx_library],
  dependencies : [vulkan_dep, fsr31_dep, dlss_dep, sdl3_dep, filesystem_dep], include_directories : [native_include, remix_public_include],
  cpp_args : cpp.get_supported_arguments('-Wno-missing-field-initializers'),
  gnu_symbol_visibility : 'hidden', pic : true)

if has_dlss
  foreach binary : dlss_binaries
    configure_file(input : '../../../third_party/dlss' / dlss_runtime_dir / binary,
      output : binary, copy : true)
  endforeach
  subdir('cnn')
endif
configure_file(input : 'dlss-runtime.json.in', output : 'dlss-runtime.json',
  configuration : {'ENABLED' : has_dlss ? 'true' : 'false'})
