// Ray portal frames (scene/portals.h): which draw of an index places it.
#include "scene/portals.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace remix::scene;
namespace {
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  std::shared_ptr<Material> portal(uint8_t index, bool overOpaque) {
    auto material = std::make_shared<Material>();
    material->rayPortal = uint8_t(index + 1);
    material->rayPortalOverOpaque = overOpaque;
    return material;
  }
  Draw placed(std::shared_ptr<Material> material, float x) {
    Draw draw;
    draw.material = std::move(material);
    draw.model = identity();
    draw.model[12] = x;
    return draw;
  }
  // The map's translation x: portal 0 at x0, portal 1 at the origin, both
  // unrotated: points map by diag(-1, -1, 1) (p - x0), so x0 itself.
  float mapX(const RayPortals &portals) { return portals.slots[0][3]; }
} // namespace
int main() {
  {
    // Alone, the first draw of an index places it.
    std::vector<Draw> draws{placed(portal(0, false), 2), placed(portal(0, false), 5), placed(portal(1, false), 0)};
    const auto portals = rayPortals(draws);
    require(portals.pairs == 1 && portals.surfaces == 3 && portals.duplicates == 1,
            "standalone portals: one pair, one duplicate");
    require(std::abs(mapX(portals) - 2) < 1e-5f, "the first standalone draw places the portal");
  }
  {
    // Surfaces opening a portal over an opaque look, then a standalone draw:
    // the standalone draw places it, whichever came first.
    std::vector<Draw> draws{placed(portal(0, true), 5), placed(portal(0, true), 7), placed(portal(0, false), 2),
                            placed(portal(1, false), 0)};
    const auto portals = rayPortals(draws);
    require(portals.pairs == 1 && portals.surfaces == 4 && portals.duplicates == 2,
            "mixed portals: one pair, two duplicates");
    require(std::abs(mapX(portals) - 2) < 1e-5f, "the standalone draw places the portal over opaque surfaces");
  }
  {
    // Only surfaces over an opaque look: the first places it, as before.
    std::vector<Draw> draws{placed(portal(0, true), 5), placed(portal(0, true), 7), placed(portal(1, false), 0)};
    const auto portals = rayPortals(draws);
    require(std::abs(mapX(portals) - 5) < 1e-5f, "without a standalone draw, the first surface places the portal");
  }
  return 0;
}
