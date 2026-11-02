// Different local context: no clustered indices, chart channels or masks.
struct LocalLightingContext { uint entries; };
LocalLightingContext prepareLocalLighting(ShadingReceiver receiver, vec3 normal,
                                         bool diffuseRequired, bool aggregate) {
  return LocalLightingContext(1u);
}
uint receiverLocalLightCount(LocalLightingContext context) { return context.entries; }
bool sampleReceiverLocalLight(uint ordinal, ShadingReceiver receiver, vec3 normal,
    LocalLightingContext context, bool frontFacingOnly, out LocalLightSample sampled) {
  sampled = LocalLightSample(false, false, vec3(0,0,1), vec3(.1), 1, 1, 1, 1, 1);
  return ordinal < context.entries;
}
LightContribution standardReceiverLocalLight(uint ordinal, ShadingReceiver receiver, vec3 normal, vec3 view,
    vec3 base, float roughness, float metallic, LocalLightingContext context, vec3 dielectricF0) {
  LocalLightSample lighting;
  if (!sampleReceiverLocalLight(ordinal, receiver, normal, context, true, lighting))
    return LightContribution(vec3(0), vec3(0));
  return shadeStandardLocalLight(lighting, normal, view, base, roughness, metallic, dielectricF0);
}
