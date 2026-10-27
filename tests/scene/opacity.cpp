#include "scene/opacity.h"
#include "scene/fixtures.h"
#include <cmath>

using namespace remix::scene;
using namespace remix::test;

int main() {
  OpacityCache cache;
  Draw draw;
  draw.doubleSided = true;
  draw.surface = std::make_shared<Surface>();
  draw.surface->vertices = {{{0, 0, 0}, {.1f, .1f}, {1, 1, 1, 1}},
                            {{1, 0, 0}, {.4f, .1f}, {1, 1, 1, 1}},
                            {{0, 1, 0}, {.1f, .4f}, {1, 1, 1, 1}}};
  draw.surface->indices = {0, 1, 2};
  draw.material = std::make_shared<Material>();
  draw.material->compare = 4;
  draw.material->reference = 1.f / 255;
  draw.material->texture = std::make_shared<Texture>();
  draw.material->texture->pixels = {
      8, 8, true, {std::vector<uint8_t>(8 * 8 * 4, 255)}};
  draw.material->texture->pixels.mips[0][4 * (6 * 8 + 6) + 3] = 0;
  const std::array<float, 4> uv{1, 1, 0, 0};
  require(cache.opaque(draw, uv),
          "Unrelated atlas cutout prevented solid tile traversal");
  require(!cache.opaque(draw, {1, 1, .5f, .5f}),
          "Animated atlas tile reused opaque coverage");
  auto original = draw;
  const auto stable = cache.partition(original, uv);
  // Material/atlas RGB animation must not repeat every triangle's proof.
  // Alpha changes outside the used rectangle can share it too.
  for (unsigned frame = 0; frame < 32; ++frame) {
    draw = original;
    draw.material = std::make_shared<Material>(*original.material);
    draw.material->tint[0] = frame / 32.f;
    draw.material->roughness = frame / 32.f;
    draw.material->emission[1] = float(frame);
    draw.material->texture =
        std::make_shared<Texture>(*original.material->texture);
    draw.material->texture->pixels.mips[0][0] = uint8_t(frame);
    draw.material->texture->pixels.mips[0][4 * (7 * 8 + 7) + 3] =
        frame & 1 ? 0 : 255;
    require(
        cache.partition(draw, uv) == stable,
        "Unrelated material or atlas animation invalidated cached coverage");
  }
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->texture = std::make_shared<Texture>(*draw.material->texture);
  draw.material->texture->pixels.mips[0][4 * (1 * 8 + 1) + 3] = 0;
  require(!cache.opaque(draw, uv) && stable->opaque(0) &&
              cache.opaque(original, uv),
          "Used alpha update reused coverage or changed a queued snapshot");
  draw = original;
  draw.blend.alphaOpTexture = 3;
  draw.blend.alpha2 = 3;
  draw.blend.factor = 0xff123456;
  const auto factor = cache.partition(draw, uv);
  draw.blend.factor = 0xff654321;
  require(cache.partition(draw, uv) == factor,
          "RGB draw factor invalidated alpha proof");
  draw.blend.factor = 0x00654321;
  require(!cache.opaque(draw, uv), "Alpha draw factor reused RGB-only proof");
  draw = original;
  draw.surface = std::make_shared<Surface>(*draw.surface);
  for (auto &v : draw.surface->vertices) {
    v.uv[0] = .1f;
    v.uv[1] = .85f;
  }
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->texture = std::make_shared<Texture>();
  draw.material->texture->pixels = {
      2, 2, true, {std::vector<uint8_t>(16, 255)}};
  draw.material->texture->pixels.mips[0][15] = 0;
  require(cache.opaque(draw, uv),
          "Dimension test failed its initial solid footprint");
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->texture = std::make_shared<Texture>(*draw.material->texture);
  draw.material->texture->pixels.width = 1;
  draw.material->texture->pixels.height = 4;
  require(!cache.opaque(draw, uv),
          "Texture dimensions reused a different alpha footprint");
  draw = original;

  draw.doubleSided = false;
  require(!cache.opaque(draw, uv),
          "Single-sided candidates bypass facing rejection");
  draw = original;
  draw.blend.enabled = true;
  require(!cache.opaque(draw, uv), "Blended sheets bypass transmission");
  draw = original;
  draw.alphaOverride = true;
  draw.compare = 0;
  require(!cache.opaque(draw, uv), "Draw alpha override reused material test");
  draw.compare = 7;
  require(cache.opaque(draw, uv),
          "Unconditional draw alpha was not recognized");
  draw = original;
  draw.blend.alphaOpTexture = 3;
  draw.blend.alpha2 = 3;
  draw.blend.factor = 0x00ffffff;
  require(!cache.opaque(draw, uv),
          "Alpha factor failed to reject solid texture");
  draw = original;
  draw.blend.alphaOpTexture = 2;
  draw.blend.alpha2 = 2;
  require(cache.opaque(draw, uv), "Vertex-alpha select ignored alpha combiner");
  draw.surface = std::make_shared<Surface>(*draw.surface);
  draw.surface->vertices[0].color[3] = 0;
  require(!cache.opaque(draw, uv), "Replacement vertex alpha reused coverage");
  draw = original;
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->texture = std::make_shared<Texture>(*draw.material->texture);
  draw.material->texture->pixels.mips[0][4 * (1 * 8 + 1) + 3] = 0;
  require(!cache.opaque(draw, uv) && cache.opaque(original, uv),
          "Texture replacement lost snapshot-specific opacity");
  draw = original;
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->tint[3] = 0;
  require(!cache.opaque(draw, uv), "Material tint was omitted from alpha test");
  draw.material = std::make_shared<Material>(*original.material);
  draw.material->transmission = true;
  require(!cache.opaque(draw, uv), "Transmission classified opaque");
  draw = original;
  for (uint8_t wrap : {1, 2, 0, 3}) {
    draw.material = std::make_shared<Material>(*original.material);
    draw.material->wrapU = wrap;
    draw.material->texture =
        std::make_shared<Texture>(*original.material->texture);
    draw.material->texture->pixels.mips[0][4 * (1 * 8 + 7) + 3] = 0;
    require(
        cache.opaque(draw, {1, 1, -.2f, 0}) == (wrap == 2 || wrap == 0),
        "Sampler addressing failed to preserve edge or wrapped alpha coverage");
  }
  // A linear footprint can reach a cutout outside the nearest footprint.
  draw = original;
  draw.surface = std::make_shared<Surface>(*draw.surface);
  for (auto &v : draw.surface->vertices) {
    v.uv[0] = .72f;
    v.uv[1] = .76f;
  }
  require(cache.opaque(draw, uv),
          "Nearest footprint unexpectedly reached neighboring cutout");
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->filter = 1;
  require(!cache.opaque(draw, uv),
          "Linear alpha footprint missed neighboring cutout");
  // A solid tile and a cutout share one original surface. Only the solid
  // triangle may bypass alpha testing; original primitive numbers stay intact.
  draw = original;
  draw.surface = std::make_shared<Surface>(*original.surface);
  for (auto v : original.surface->vertices) {
    v.uv[0] += .5f;
    v.uv[1] += .5f;
    draw.surface->vertices.push_back(v);
  }
  draw.surface->indices = {3, 4, 5, 0, 1, 2};
  auto partition = cache.partition(draw, uv);
  require(partition->key[0] == 2 && !partition->opaque(0) &&
              partition->opaque(1) &&
              partition->reasons[OpacityPartition::Solid] == 1 &&
              partition->reasons[OpacityPartition::TextureCutout] == 1,
          "Mixed atlas surface was not classified per original triangle");
  require(cache.partition(draw, uv) == partition,
          "Immutable coverage was not cached");
  draw.doubleSided = false;
  const auto oneSided = cache.partition(draw, uv);
  require(oneSided->reasons[OpacityPartition::OneSided] == 2,
          "Facing eligibility reused a double-sided partition");
  // Hardware face culling: a one-sided surface takes the double-sided proof
  // (traversal rejects its back faces); cached apart from the default.
  cache.setOneSidedOpaque(true);
  const auto culled = cache.partition(draw, uv);
  require(culled != oneSided && culled->key == partition->key &&
              culled->reasons[OpacityPartition::Solid] == 1 &&
              culled->reasons[OpacityPartition::TextureCutout] == 1 &&
              !culled->reasons[OpacityPartition::OneSided],
          "Hardware face culling did not prove one-sided coverage per triangle");
  require(cache.find(draw, uv) == culled, "Hardware face culling coverage was not cached");
  draw.doubleSided = true;
  require(cache.partition(draw, uv) == partition,
          "Hardware face culling changed double-sided coverage");
  draw.doubleSided = false;
  auto blended = draw;
  blended.blend.enabled = true;
  require(!cache.opaque(blended, uv), "Hardware face culling made a blended sheet opaque");
  blended = draw;
  blended.material = std::make_shared<Material>(*draw.material);
  blended.material->transmission = true;
  require(!cache.opaque(blended, uv), "Hardware face culling made transmission opaque");
  blended = draw;
  blended.ignoreAlpha = true;
  require(cache.opaque(blended, uv), "Hardware face culling missed an unconditional one-sided surface");
  blended.alphaOverride = true;
  blended.ignoreAlpha = false;
  blended.compare = 0;
  require(!cache.opaque(blended, uv), "Hardware face culling made a never-passing surface opaque");
  cache.setOneSidedOpaque(false);
  require(cache.partition(draw, uv) == oneSided && cache.find(draw, uv) == oneSided,
          "Disabling hardware face culling did not restore one-sided coverage");
  draw.doubleSided = true;
  draw.surface = std::make_shared<Surface>(*draw.surface);
  draw.surface->vertices[0].color[3] = 0;
  draw.blend.alphaOpTexture = 3;
  draw.blend.alpha1 = 1;
  draw.blend.alpha2 = 2;
  require(!cache.partition(draw, uv)->opaque(1) && partition->opaque(1),
          "Vertex-alpha replacement changed queued partition ownership");
  // Check every accepted proof against sampled triangle interiors and the
  // sampler's integer addressing, across negative UVs and mirror/repeat seams.
  auto address = [](int texel, int size, unsigned wrap) {
    if (wrap == 3 && (texel < 0 || texel >= size))
      return -1;
    if (wrap == 0 || wrap == 3)
      return std::clamp(texel, 0, size - 1);
    const int period = wrap == 2 ? size * 2 : size;
    const int value = (texel % period + period) % period;
    return value < size ? value : period - 1 - value;
  };
  for (unsigned wrap : {1, 2, 0, 3})
    for (unsigned filter : {0, 1}) {
      draw = original;
      draw.material = std::make_shared<Material>(*original.material);
      draw.material->wrapU = draw.material->wrapV = wrap;
      draw.material->filter = filter;
      for (float scale : {.1f, 1.f, 3.f, -1.f})
        for (int offset = -24; offset <= 24; ++offset) {
          const std::array<float, 4> transform{scale, scale, offset / 8.f,
                                               offset / 13.f};
          if (!cache.opaque(draw, transform))
            continue;
          for (unsigned i = 0; i <= 12; ++i)
            for (unsigned j = 0; i + j <= 12; ++j) {
              const float u = (.1f + .3f * i / 12.f) * scale + transform[2];
              const float v = (.1f + .3f * j / 12.f) * scale + transform[3];
              const int x = int(std::floor(u * 8 - (filter ? .5f : 0.f)));
              const int y = int(std::floor(v * 8 - (filter ? .5f : 0.f)));
              for (unsigned dy = 0; dy <= filter; ++dy)
                for (unsigned dx = 0; dx <= filter; ++dx) {
                  const int tx = address(x + dx, 8, wrap),
                            ty = address(y + dy, 8, wrap);
                  require(
                      tx >= 0 && ty >= 0 &&
                          draw.material->texture->pixels
                                  .mips[0][4 * (ty * 8 + tx) + 3] == 255,
                      "Wrapped opacity proof omitted a reachable alpha texel");
                }
            }
        }
    }
  cache.prune();
}
