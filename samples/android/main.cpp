#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#define main remixSampleMain
#include "../remix-native/main.cpp"
#undef main

extern "C" __attribute__((visibility("default"))) int SDL_main(int argc, char **argv) {
  if (argc < 3 || chdir(argv[1]) != 0) {
    return 2;
  }
  std::freopen("sample.log", "w", stdout);
  dup2(fileno(stdout), fileno(stderr));
  setenv("SDL_ORIENTATIONS", "LandscapeLeft LandscapeRight", 1);
  if (access("validation", F_OK) == 0) {
    setenv("REMIX_VULKAN_VALIDATION", "1", 1);
  }
  const int result = remixSampleMain(argc - 1, argv + 1);
  std::printf("Android sample exit: %d\n", result);
  std::fflush(stdout);
  return result;
}
