usd_root = get_option('usd_root')
if usd_root == ''
  error('usd=true requires usd_root pointing to OpenUSD 25.11')
endif
usd_deps = []
foreach lib : ['usdUtils', 'usdGeom', 'usdShade', 'usdLux', 'usdSkel', 'usd', 'sdf', 'ar', 'gf', 'vt', 'tf', 'work', 'plug', 'ts', 'pcp', 'arch', 'js', 'trace']
  usd_deps += cpp.find_library('usd_' + lib, dirs : [usd_root / 'lib'])
endforeach
# clang-cl takes -isystem only through /clang: (as plain -isystem the headers
# are not system headers, and OpenUSD's and TBB's warnings fail a werror build).
usd_system = cpp.get_id() == 'clang-cl' ? '/clang:-isystem' : '-isystem'
usd_args = [usd_system + usd_root / 'include']
if get_option('usd_tbb_root') != ''
  usd_args += [usd_system + get_option('usd_tbb_root') / 'include']
  # oneTBB's import library on Windows is tbb12.
  usd_tbb = cpp.find_library('tbb12', dirs : [get_option('usd_tbb_root') / 'lib'], required : false)
  if not usd_tbb.found()
    usd_tbb = cpp.find_library('tbb', dirs : [get_option('usd_tbb_root') / 'lib'])
  endif
  usd_deps += usd_tbb
else
  usd_deps += dependency('tbb')
endif
if get_option('usd_gnu_stl')
  usd_args += ['-DARCH_HAS_GNU_STL_EXTENSIONS']
endif
if get_option('json_include') != ''
  usd_args += ['-I' + get_option('json_include')]
endif
if not cpp.compiles('#include <pxr/pxr.h>
#if PXR_VERSION != 2511
#error OpenUSD 25.11 required
#endif
#include <nlohmann/json.hpp>
int main() {}', args : usd_args)
  error('USD loader requires OpenUSD 25.11 and nlohmann/json headers')
endif
usd_library = static_library('remix_usd', 'scene.cpp', 'skinning.cpp', 'capture_categories.cpp', 'loader.cpp', 'submission.cpp', 'mods.cpp', 'capture.cpp', 'capture_writer.cpp',
  dependencies : usd_deps + [filesystem_dep, threads_dep], include_directories : [remix_public_include, native_include],
  cpp_args : usd_args + ['-DREMIX_HAS_USD=1'], gnu_symbol_visibility : 'hidden', pic : true)
usd_libraries += usd_library
shell_deps += usd_deps
shell_args += usd_args + ['-DREMIX_HAS_USD=1']
