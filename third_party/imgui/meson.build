imgui_include = include_directories('.', 'backends')
imgui_core = static_library('imgui_core',
  ['imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp', 'backends/imgui_impl_sdl3.cpp'],
  include_directories : imgui_include, dependencies : sdl3_dep,
  gnu_symbol_visibility : 'hidden', pic : true, override_options : ['warning_level=0', 'werror=false'])
imgui_dep = declare_dependency(link_with : imgui_core, include_directories : imgui_include, dependencies : sdl3_dep)
if not metal_backend
  imgui_vulkan = static_library('imgui_vulkan', 'backends/imgui_impl_vulkan.cpp',
    include_directories : imgui_include, dependencies : vulkan_dep,
    gnu_symbol_visibility : 'hidden', pic : true, override_options : ['warning_level=0', 'werror=false'])
endif
install_data('LICENSE.txt', install_dir : get_option('datadir') / 'remix/licenses/imgui')
