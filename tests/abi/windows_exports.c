#include <remix/remix_c.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc != 3)
    return 1;
  HMODULE library = LoadLibraryA(argv[1]);
  if (!library) {
    fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
    return 1;
  }
  FILE *exports = fopen(argv[2], "r");
  if (!exports)
    return 1;
  char name[256];
  while (fgets(name, sizeof(name), exports)) {
    name[strcspn(name, "\r\n")] = 0;
    if (*name && !GetProcAddress(library, name)) {
      fprintf(stderr, "Missing export: %s\n", name);
      return 1;
    }
  }
  fclose(exports);
  typedef int(REMIXAPI_CALL * Begin)(const char *, int *, int);
  typedef void(REMIXAPI_CALL * WindowSize)(float *, float *);
  Begin begin = (Begin)GetProcAddress(library, "remixapi_imgui_Begin");
  WindowSize windowSize = (WindowSize)GetProcAddress(library, "remixapi_imgui_GetWindowSize");
  float value = 12;
  if (!begin || !windowSize || begin(NULL, NULL, 0))
    return 1;
  windowSize(&value, NULL);
  if (value != 0)
    return 1;
  FreeLibrary(library);
  puts("Windows exports, library loading and inert compatibility widgets passed");
  return 0;
}
