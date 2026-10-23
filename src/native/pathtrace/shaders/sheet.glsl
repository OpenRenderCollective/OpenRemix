// Hit-local optical depth preserves the texture footprint of this intersection.
// The descriptor index keeps full-precision material parameters in the shared
// optional table instead of widening every CPU/GPU surface record.
void loadSheet(inout Hit hit, Surface surface, vec2 uv) {
  hit.sheetIndex = 0u;
  hit.sheetDepth = vec3(0);
  if (!hit.blended || (surface.material.x & 256u) == 0u || frame.sheetMaterials == 0)
    return;
  hit.sheetIndex = surface.textureState.x + 1u;
  SheetMaterial sheet = SheetMaterials(frame.sheetMaterials).data[hit.sheetIndex - 1u];
  vec2 tintUv = sheetTintUv(sheet, uv);
  vec3 tint = materialTexel(surface, surface.textureState.x, tintUv).rgb;
  hit.sheetDepth = min(sheet.depth.rgb - log(clamp(tint, vec3(1e-6), vec3(1))) * sheet.parameters.z, vec3(80));
  hit.pbr = true;
  hit.rm.y = 0; // A transmitting dielectric has no metallic or diffuse lobe.
  hit.color.a = 1; // The BSDF carries reflection; transmission is composed separately.
}
vec4 sheetParameters(Hit hit) {
  return SheetMaterials(frame.sheetMaterials).data[hit.sheetIndex - 1u].parameters;
}
DielectricSheet sheetOptics(Hit hit, float cosine) {
  return dielectricSheet(hit.sheetDepth, 1, cosine, 1, sheetParameters(hit).x);
}
float sheetReflectionProbability(DielectricSheet optics) {
  const vec3 luma = vec3(.2126, .7152, .0722);
  float reflected = dot(optics.reflection, luma);
  float transmitted = dot(optics.transmission, luma);
  return reflected / max(reflected + transmitted, 1e-20);
}
vec3 offsetRay(vec3 point, vec3 normal);
vec3 sheetOrigin(Hit hit, vec3 direction) {
  vec3 point = hit.p;
  if (hit.sheetIndex != 0u) {
    vec4 parameters = sheetParameters(hit);
    point += dielectricSheetPath(direction, hit.n, parameters.y, 1, parameters.x).exitOffset;
  }
  return offsetRay(point, -hit.ng);
}
