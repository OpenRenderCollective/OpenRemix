# NUMOS passes and the adapter passes, compiled with the pinned slangc by the build
# (scripts/shaders/sky.py): XZ streams in embedded.bin, which embedded_data.cpp embeds.
sky_shaders = custom_target('sky-shaders',
  output : ['embedded.h', 'embedded.bin', 'embedded_data.cpp', 'adapter_passes.inc'],
  command : shader_generate + ['sky'], depfile : 'embedded.d',
  depend_files : shader_generator_files + files('../../../../scripts/shaders/sky.py'), env : shader_env)
