#include <remix/remix.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }

  auto loadResult = remix::lib::loadRemixLibraryAndInitialize(argv[1]);
  if (!loadResult) {
    return 2;
  }

  remix::Interface &interface = *loadResult;
  auto featureResult = interface.GetFeatureAvailability();
  if (!featureResult || featureResult->availableFeatures != 0) {
    return 3;
  }

  auto windowResult = interface.GetWindowState();
  if (windowResult.status() != REMIXAPI_ERROR_CODE_NOT_INITIALIZED) {
    return 4;
  }

  auto shutdownResult = remix::lib::shutdownAndUnloadRemixDll(interface);
  return shutdownResult ? 0 : 5;
}
