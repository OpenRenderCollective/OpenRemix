# FSR (fsr.h): the ffx-api DLLs on Windows, FSR 3.1.4 linked elsewhere and in
# Windows builds made with -Dfsr_linked=true.
fsr_sources = ['fsr.cpp']
if fsr_ffx
  fsr_sources += ['fsr_ffx.cpp', 'fsr_frame_generation_ffx.cpp', 'ffx_library.cpp']
endif
if fsr_linked
  fsr_sources += ['fsr_linked.cpp', 'fsr_frame_generation.cpp', 'sdk_backend.cpp']
endif
# XeSS (xess.h): Intel's libxess.dll, Windows only.
xess_sources = has_xess ? ['xess.cpp'] : []
upscale_library = static_library('remix_upscale', 'renderer.cpp', 'ngx.cpp', 'neural_uplift.cpp',
  fsr_sources, xess_sources, upscale_shaders,
  link_with : [pipeline_library, postfx_library],
  dependencies : [vulkan_dep, fsr_dep, dlss_dep, xess_dep, sdl3_dep, filesystem_dep], include_directories : [native_include, remix_public_include],
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
# Beside the build's remix.dll's upscale/ folder, where ffx_library.cpp looks.
if fsr_ffx
  foreach binary : fidelityfx_binaries
    configure_file(input : fidelityfx_runtime_dir / binary, output : binary, copy : true)
  endforeach
endif
# Likewise libxess.dll, where xess.cpp looks.
if has_xess
  foreach binary : xess_binaries
    configure_file(input : xess_runtime_dir / binary, output : binary, copy : true)
  endforeach
endif
