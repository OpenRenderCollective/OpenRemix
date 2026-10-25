sun_depth_test_shaders = custom_target('sun_depth_test-shaders',
  output : 'embedded.h', command : shader_generate + ['sun_depth_test'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)
