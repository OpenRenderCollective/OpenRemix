# SPIRV-Cross's MSL backend (Metal builds): the host passes' shaders, SPIR-V
# the host registers at run time, translated to MSL (src/native/metal/host_passes.mm).
spirv_cross = static_library('spirv_cross',
  ['spirv_cross.cpp', 'spirv_parser.cpp', 'spirv_cross_parsed_ir.cpp', 'spirv_cfg.cpp', 'spirv_glsl.cpp',
   'spirv_msl.cpp'],
  cpp_args : ['-DSPIRV_CROSS_NAMESPACE_OVERRIDE=remix_spirv_cross'],
  gnu_symbol_visibility : 'hidden', pic : true, override_options : ['warning_level=0', 'werror=false'])
spirv_cross_dep = declare_dependency(link_with : spirv_cross, include_directories : include_directories('.', is_system : true),
  compile_args : ['-DSPIRV_CROSS_NAMESPACE_OVERRIDE=remix_spirv_cross'])
install_data('LICENSE', rename : 'SPIRV-Cross.txt', install_dir : get_option('datadir') / 'remix/licenses')
