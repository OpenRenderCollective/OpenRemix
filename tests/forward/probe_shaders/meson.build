probe_test_shaders = custom_target('probe_test-shaders',
  output : 'embedded.h', command : shader_generate + ['probe_test'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)
