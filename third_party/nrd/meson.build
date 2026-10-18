nrd_include = include_directories('Include', is_system : true)
nrd_dep = declare_dependency(include_directories : nrd_include)
if get_option('nrd')
  nrd_args = ['-DNRD_STATIC_LIBRARY=1', '-DNRD_EMBEDS_SPIRV_SHADERS=0',
    '-DNRD_EMBEDS_DXIL_SHADERS=0', '-DNRD_EMBEDS_DXBC_SHADERS=0',
    '-DSPIRV_SREG_OFFSET=0', '-DSPIRV_BREG_OFFSET=2',
    '-DSPIRV_UREG_OFFSET=3', '-DSPIRV_TREG_OFFSET=20']
  if host_machine.cpu_family() == 'x86_64'
    nrd_args += cpp.get_id() == 'clang-cl' ? ['/clang:-mssse3'] : ['-mssse3']
  endif
  # sse2neon (arm64) announces itself with #warning, an error under werror.
  nrd_args += cpp.get_supported_arguments('-Wno-#warnings')
  nrd_cpu = static_library('nrd',
    'Source/InstanceImpl.cpp', 'Source/Reblur.cpp', 'Source/Reference.cpp',
    'Source/Relax.cpp', 'Source/Sigma.cpp', 'Source/Timer.cpp', 'Source/Wrapper.cpp',
    include_directories : [nrd_include, include_directories('External/MathLib',
      'External/ShaderMake', 'External/sse2neon', is_system : true)],
    cpp_args : nrd_args, gnu_symbol_visibility : 'hidden',
    override_options : ['warning_level=0'], pic : true)
  nrd_dep = declare_dependency(link_with : nrd_cpu, include_directories : nrd_include,
    compile_args : ['-DNRD_STATIC_LIBRARY=1', '-DREMIX_HAS_NRD=1'])
endif
install_data('LICENSE.txt', rename : 'NRD.txt', install_dir : get_option('datadir') / 'remix/licenses')
install_data('External/MathLib/LICENSE.txt', rename : 'MathLib.txt', install_dir : get_option('datadir') / 'remix/licenses')
install_data('External/ShaderMake/LICENSE.txt', rename : 'ShaderMake.txt', install_dir : get_option('datadir') / 'remix/licenses')
install_data('External/sse2neon/LICENSE', rename : 'sse2neon.txt', install_dir : get_option('datadir') / 'remix/licenses')
