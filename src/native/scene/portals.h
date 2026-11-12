#pragma once
#include "scene/types.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <remix/remix_c.h>
#include <vector>

namespace remix::scene {
  // remixapi_MaterialInfoPortalEXT ray portals (remixapi_GetRayPortalCapabilities
  // reports the count): portal i's partner is i ^ 1. A portal's frame is its
  // instance transform T; in the mesh's own space the opening lies in the
  // plane x = 0, its front (where rays enter) faces +X and +Z is up. A ray
  // meeting the front of portal A whose partner B is drawn this frame goes on
  // from B: points map by T_B diag(-1, -1, 1) T_A^-1, directions by its
  // linear part. Material::rayPortal holds the index + 1.
  inline constexpr uint32_t rayPortalSlots = 8;

  // The frame's portals, by index (trace::Frame::rayPortals): the rows of the
  // map to the partner (3 x 4), then the world normal of the portal's front
  // with flags in w (1: the pair is open, both drawn; plus 2 when it moved
  // since the last frame, set by the tracer).
  struct RayPortals {
    std::array<std::array<float, 16>, rayPortalSlots> slots = {};
    // The opening's plane: dot(front normal, p) at the plane x = 0 (world
    // units; trace::Frame::rayPortalPlanes). The host's mesh may stand off
    // it (the Portal 2 wrapper's lies at x = +0.05, off the wall).
    std::array<float, rayPortalSlots> planes = {};
    uint32_t surfaces = 0;   // traced portal draws this frame
    uint32_t pairs = 0;      // pairs with both portals drawn
    uint32_t duplicates = 0; // draws of an index already taken (the first counts)
  };

  // Collects the frame's portals from its draws (traced world draws only: a
  // view model or a reflection-only body does not open a portal). In double,
  // since the map composes two transforms and an inverse. An index's frame is
  // its first draw's, a standalone portal's before one extending an opaque
  // material: many surfaces of a look of their own (screens, windows) can open
  // one portal placed by a dedicated draw.
  inline RayPortals rayPortals(const std::vector<Draw> &draws) {
    RayPortals out;
    std::array<const Draw *, rayPortalSlots> placed = {};
    for (const auto &draw : draws) {
      if (!draw.material || !draw.material->rayPortal || !draw.traced() || draw.viewModel || draw.reflectionOnly)
        continue;
      ++out.surfaces;
      auto &slot = placed[draw.material->rayPortal - 1u];
      if (!slot)
        slot = &draw;
      else {
        ++out.duplicates;
        if (slot->material->rayPortalOverOpaque && !draw.material->rayPortalOverOpaque)
          slot = &draw;
      }
    }
    // Affine 3 x 4 matrices (row-major) from the column-major Matrix.
    using Affine = std::array<std::array<double, 4>, 3>;
    auto affine = [](const Matrix &m) {
      Affine a{};
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
          a[r][c] = m[c * 4 + r];
      return a;
    };
    auto invert = [](const Affine &a, Affine &out) {
      const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                         a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                         a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
      if (!(std::abs(det) > 1e-12) || !std::isfinite(det))
        return false;
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
          const int r1 = (c + 1) % 3, r2 = (c + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
          out[r][c] = (a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1]) / det;
        }
      for (int r = 0; r < 3; ++r)
        out[r][3] = -(out[r][0] * a[0][3] + out[r][1] * a[1][3] + out[r][2] * a[2][3]);
      return true;
    };
    for (uint32_t pair = 0; pair < rayPortalSlots / 2; ++pair) {
      const Draw *a = placed[2 * pair], *b = placed[2 * pair + 1];
      if (!a || !b)
        continue;
      const Affine ta = affine(a->model), tb = affine(b->model);
      Affine ia, ib;
      if (!invert(ta, ia) || !invert(tb, ib))
        continue;
      ++out.pairs;
      for (unsigned side = 0; side < 2; ++side) {
        const Affine &to = side ? ta : tb, &from = side ? ib : ia;
        // to * diag(-1, -1, 1) * from.
        Affine flipped = to;
        for (int r = 0; r < 3; ++r)
          flipped[r][0] = -flipped[r][0], flipped[r][1] = -flipped[r][1];
        auto &slot = out.slots[2 * pair + side];
        for (int r = 0; r < 3; ++r) {
          for (int c = 0; c < 4; ++c) {
            double value = c == 3 ? flipped[r][3] : 0;
            for (int k = 0; k < 3; ++k)
              value += flipped[r][k] * from[k][c];
            slot[r * 4 + c] = float(value);
          }
        }
        // The front's normal: the plane x = 0's, row 0 of the inverse (the
        // inverse transpose maps normals; equal to +X's image when rigid).
        const double n[3] = {from[0][0], from[0][1], from[0][2]};
        const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        slot[12] = float(n[0] / length), slot[13] = float(n[1] / length), slot[14] = float(n[2] / length);
        slot[15] = 1;
        const Affine &own = side ? tb : ta;
        out.planes[2 * pair + side] = float((n[0] * own[0][3] + n[1] * own[1][3] + n[2] * own[2][3]) / length);
      }
    }
    return out;
  }
} // namespace remix::scene
