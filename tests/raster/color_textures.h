// Encoded color resources must agree with the path tracer before lighting.
// Keep data/HDR resources linear, and never apply RGB transfer to alpha.
void colorTextureTests() {
  for (uint32_t samples : {1u, 4u}) {
    Target target;
    auto &r = target.raster;
    remix::forward::Options options;
    options.samples = samples;
    options.ambient = 1;
    options.prism = options.gamma22 = false;
    options.shadows = options.localShadows = options.ssr = options.water = false;
    options.baking.enabled = false;
    options.baking.reflectionProbes = 0;
    target.enableForward(options);
    for (uint32_t kind = 0; kind < 3; ++kind) {
      const uint8_t bytes[] = {128, 64, 192, 160};
      const float linear[] = {.25f, .5f, .75f, .502f};
      remixapi_TextureInfo texture{};
      texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
      texture.hash = 0xc010 + kind;
      texture.width = texture.height = texture.depth = texture.mipLevels = 1;
      texture.format = kind == 0 ? REMIXAPI_FORMAT_R8G8B8A8_SRGB
          : kind == 1 ? REMIXAPI_FORMAT_R8G8B8A8_UNORM : REMIXAPI_FORMAT_R32G32B32A32_SFLOAT_EXT;
      texture.data = kind == 2 ? static_cast<const void *>(linear) : bytes;
      texture.dataSize = kind == 2 ? sizeof(linear) : sizeof(bytes);
      remixapi_TextureHandle handle;
      r.createTexture(texture, handle);
#if defined(_WIN32)
      const auto name = std::wstring(kind == 0 ? L"0xc010" : kind == 1 ? L"0xc011" : L"0xc012");
#else
      const auto name = std::string(kind == 0 ? "0xc010" : kind == 1 ? "0xc011" : "0xc012");
#endif
      const auto mesh = quad(r, 0xc030 + kind,
          material(r, 0xc020 + kind, 1, 1, 1, 1, name.c_str(), 4));
      camera(r);
      r.drawInstance(instance(mesh));
      const auto image = target.render();
      std::array<int, 3> expected{};
      for (uint32_t c = 0; c < 3; ++c) {
        const float value = kind == 2 ? linear[c]
            : kind == 0 ? std::pow(float(bytes[c]) / 255, 2.2f) : float(bytes[c]) / 255;
        expected[c] = int(std::lround(value * 255));
      }
      pixel(image, 32, 32, expected[0], expected[1], expected[2],
          "Forward+ encoded/linear/HDR color and unchanged alpha coverage");
      remixapi_MaterialInfoOpaqueEXT opaque{};
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.opacityConstant = 1;
      opaque.roughnessConstant = 1;
      opaque.alphaTestType = 7;
      remixapi_MaterialInfo emission{};
      emission.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      emission.pNext = &opaque;
      emission.hash = 0xc050 + kind;
      emission.emissiveIntensity = 1;
      emission.emissiveColorConstant = {1, 1, 1};
      emission.emissiveTexture = name.c_str();
      remixapi_MaterialHandle glow;
      r.createMaterial(emission, glow);
      camera(r);
      r.drawInstance(instance(quad(r, 0xc060 + kind, glow)));
      pixel(target.render(), 32, 32, expected[0], expected[1], expected[2],
          "Forward+ emission uses the same color transfer without decoding linear HDR");
    }
  }
  require(errors.load() == 0, "Color texture sampling is validation clean");
}
