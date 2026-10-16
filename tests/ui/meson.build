text_test = executable('remix-text-input-test', 'text_input.cpp', dependencies : imgui_dep)
test('SDL text editing and capture', text_test)
profile_ui_test = executable('remix-profile-ui-test', 'profiles.cpp',
  '../../src/native/runtime/settings.cpp', '../../src/native/runtime/profiles.cpp',
  '../../src/native/ui/profile_actions.cpp', '../../src/native/ui/profiles.cpp', '../../src/native/ui/style.cpp', '../../src/native/ui/panel.cpp',
  '../../src/native/ui/mods.cpp',
  '../../src/native/api/imgui_compat.cpp',
  include_directories : native_include, dependencies : [imgui_dep, filesystem_dep],
  cpp_args : cpp.get_supported_arguments('-Wno-unused-parameter')) # as the runtime's shell_args
test('Profile dialogs and input capture', profile_ui_test, args : ['@CONFIG_DIR@'])
