# JPEG XR encoder/decoder core and glue (HDR screenshots). Upstream sources are
# unmodified; portability comes from defines. Windows x64 builds the ANSI path
# too: it sizes pointer integers from __LP64__, and remix_compat.h resolves the
# SDK clashes. Consumers define __ANSI__ before including JXRGlue.h.
jxrlib_c_args = ['-D__ANSI__', '-DDISABLE_PERF_MEASUREMENT', '-w', '-Wno-implicit-function-declaration']
if host_machine.system() == 'windows'
  jxrlib_c_args += ['-D__LP64__=1', '-FI' + meson.current_source_dir() / 'remix_compat.h']
endif
jxrlib_include = include_directories('common/include', 'image/sys', 'jxrgluelib', is_system : true)
jxrlib = static_library('jxrlib',
  ['image/sys/adapthuff.c', 'image/sys/image.c', 'image/sys/strcodec.c',
   'image/sys/strPredQuant.c', 'image/sys/strTransform.c', 'image/sys/perfTimerANSI.c',
   'image/decode/decode.c', 'image/decode/postprocess.c', 'image/decode/segdec.c',
   'image/decode/strdec.c', 'image/decode/strInvTransform.c', 'image/decode/strPredQuantDec.c',
   'image/decode/JXRTranscode.c',
   'image/encode/encode.c', 'image/encode/segenc.c', 'image/encode/strenc.c',
   'image/encode/strFwdTransform.c', 'image/encode/strPredQuantEnc.c',
   'jxrgluelib/JXRGlue.c', 'jxrgluelib/JXRMeta.c', 'jxrgluelib/JXRGluePFC.c',
   'jxrgluelib/JXRGlueJxr.c'],
  include_directories : jxrlib_include, c_args : jxrlib_c_args,
  gnu_symbol_visibility : 'hidden', pic : true, override_options : ['warning_level=0', 'werror=false'])
jxrlib_dep = declare_dependency(link_with : jxrlib, include_directories : jxrlib_include)
install_data('LICENSE', rename : 'jxrlib.txt', install_dir : get_option('datadir') / 'remix/licenses')
