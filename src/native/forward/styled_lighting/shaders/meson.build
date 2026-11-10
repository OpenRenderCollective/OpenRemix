styled_lighting_shaders = custom_target('styled-lighting-shaders',
  output : 'embedded.h', command : shader_generate + ['styled_lighting'],
  depfile : 'embedded.d', depend_files : shader_generator_files, env : shader_env)
