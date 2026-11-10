style_test_shaders = custom_target('style_test-shaders',
  output : 'embedded.h', command : shader_generate + ['style_test'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)
