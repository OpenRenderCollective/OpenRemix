fs = import('fs')
numos_dep = declare_dependency()
numos_found = fs.exists('numos-sky/src/renderer.cpp')
if get_option('sky').enabled() and not numos_found
  error('-Dsky=enabled needs the NUMOS submodule: git submodule update --init third_party/numos/numos-sky')
endif
numos_enabled = numos_found and not get_option('sky').disabled()
if get_option('sky').auto() and not numos_found
  message('NUMOS sky disabled: third_party/numos/numos-sky is not checked out.')
endif
if numos_enabled
  numos_include = include_directories('numos-sky/include', 'numos-sky/shaders', is_system : true)
  numos_core = static_library('numos',
    'numos-sky/src/layout.cpp', 'numos-sky/src/log.cpp', 'numos-sky/src/settings.cpp',
    'numos-sky/src/shaders.cpp', 'numos-sky/src/celestial.cpp', 'numos-sky/src/renderer.cpp',
    'numos-sky/src/renderer_api.cpp', 'numos-sky/src/weather.cpp', 'numos-sky/src/seed.cpp',
    'numos-sky/src/ground_field.cpp',
    include_directories : [numos_include, include_directories('numos-sky/src')],
    gnu_symbol_visibility : 'hidden', pic : true,
    override_options : ['cpp_std=c++17', 'warning_level=0', 'werror=false'])
  numos_dep = declare_dependency(link_with : numos_core, include_directories : numos_include,
    compile_args : ['-DREMIX_HAS_NUMOS=1'])
  licenses = get_option('datadir') / 'remix/licenses'
  install_data('numos-sky/LICENSE', rename : 'NUMOS.txt', install_dir : licenses)
  install_data('numos-sky/NOTICE', rename : 'NUMOS-NOTICE.txt', install_dir : licenses)
  install_data('numos-sky/THIRD_PARTY_NOTICES.md', rename : 'NUMOS-THIRD_PARTY_NOTICES.md', install_dir : licenses)
  install_data('README.remix.md', rename : 'NUMOS-provenance.md', install_dir : licenses)
  foreach name : ['LICENSE-MIT-NVIDIA.txt', 'LICENSE-MIT-contributors.txt',
                  'LICENSE-MIT-Blender-sky.txt', 'LICENSE-DXVK-zlib.txt']
    install_data('numos-sky/licenses' / name, rename : 'NUMOS-' + name, install_dir : licenses)
  endforeach
endif
