forward_test_shaders = custom_target('forward_test-shaders',
  output : 'embedded.h', command : shader_generate + ['forward_test'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)
