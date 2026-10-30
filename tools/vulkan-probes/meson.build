glslang = find_program('glslangValidator')
spirv_val = find_program('spirv-val')
shader_outputs = []
foreach shader : ['probe.comp', 'probe.rgen', 'probe.rchit', 'probe.rmiss']
  shader_outputs += custom_target(shader,
    input : 'shaders' / shader,
    output : shader + '.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.2', '@INPUT@', '-o', '@OUTPUT@'],
    build_by_default : true)
  test(shader + ' SPIR-V validation', spirv_val,
    args : ['--target-env', 'vulkan1.2', shader_outputs[-1]])
endforeach

bindless_shader = custom_target('probe_bindless.comp',
  input : 'shaders/probe.comp', output : 'probe_bindless.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.2', '-DUSE_BINDLESS=1', '@INPUT@', '-o', '@OUTPUT@'],
  build_by_default : true)
shader_outputs += bindless_shader
test('bindless SPIR-V validation', spirv_val,
  args : ['--target-env', 'vulkan1.2', bindless_shader])

probe = executable('remix-vulkan-probe', 'remix_vulkan_probe.cpp',
  dependencies : vulkan_dep, cpp_args : ['-Wno-missing-field-initializers'])
if get_option('gpu_tests')
foreach mode : ['query', 'pipeline', 'bindless', 'semaphore']
  test('Vulkan probe ' + mode, probe,
    args : [meson.current_build_dir(), mode],
    depends : shader_outputs,
    timeout : 120)
endforeach

endif
