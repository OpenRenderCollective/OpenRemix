// Smooth, measured dielectric boundaries. Ripple normals remain a live effect.
#include "../../shaders/dielectric.glsl"
struct Medium {
  vec3 extinction;
  float index;
};
Medium airMedium() { return Medium(vec3(0), 1); }
Medium materialMedium(RayMaterial material) { return Medium(material.optics.rgb, material.optics.w); }
bool dielectric(RayMaterial material) { return (material.flags.w & 4u) != 0u; }
bool thinDielectric(RayMaterial material) { return (material.flags.w & 16u) != 0u; }
DielectricSheet materialSheet(RayMaterial material, vec2 uv, float cosine, float incidentIndex) {
  vec3 tint = sampleMap(material.textures.x, uv, material.flags.xyz).rgb;
  vec3 tintDepth = -log(clamp(tint, vec3(1e-6), vec3(1))) * uintBitsToFloat(material.textures.w);
  vec3 opticalDepth = min(material.optics.rgb + tintDepth, vec3(80));
  return dielectricSheet(opticalDepth, 1, cosine, incidentIndex, material.optics.w);
}
vec3 attenuation(Medium medium, float distance) { return beerLambert(medium.extinction, distance); }
// Locate a containing boundary once per shading point, rather than per sample.
// A first-hit exit in any axis is evidence of being inside. Opaque geometry
// blocks that axis, preventing an open pool from extending below its floor.
// Disjoint bodies are supported; overlapping/nested media need a volume stack.
Medium initialMedium(vec3 position, inout uint seed) {
  Medium result = airMedium();
  // Thin sheets affect visibility but cannot contain a receiver. Skip all six
  // containment directions when the scene has no volumetric boundaries.
  if ((rayFrame.control.w & 64u) == 0u)
    return result;
  float nearest = rayFrame.trace.x;
  for (uint axis = 0; axis < 6u; ++axis) {
    vec3 direction = vec3(0);
    direction[axis / 2u] = (axis & 1u) == 0u ? 1 : -1;
    vec3 origin = position;
    float travelled = 0;
    for (uint boundary = 0u; boundary < 32u; ++boundary) {
      Hit hit;
      if (!traceRay(origin, direction, rayFrame.trace.x - travelled, seed, hit))
        break;
      RayMaterial material = rayMaterials[rayTriangles[hit.primitive].w];
      if (thinDielectric(material)) {
        // A pane neither contains this point nor hides a body's exit behind it.
        float advance = hit.distance + rayFrame.trace.y;
        travelled += advance;
        if (travelled >= rayFrame.trace.x)
          break;
        origin += direction * advance;
        continue;
      }
      if (!dielectric(material) || travelled + hit.distance >= nearest)
        break;
      vec2 uv;
      vec4 color;
      vec3 normal, geometric;
      attributes(hit.primitive, hit.bary, uv, color, normal, geometric);
      if (dot(normal, direction) > 0) {
        result = materialMedium(material);
        nearest = travelled + hit.distance;
      }
      break;
    }
  }
  return result;
}
// Straight shadow connections retain Beer-Lambert and interface Fresnel loss.
// They omit refractive caustic focusing; continuation rays below use Snell's law.
vec3 visibility(vec3 origin, vec3 direction, float maximum, inout uint seed, Medium medium) {
  if (maximum <= rayFrame.trace.y)
    return vec3(0);
  // Without dielectric interfaces, any covered blocker fully occludes the
  // connection. Let hardware stop at its first accepted hit. Cutout and thin
  // coverage tests still run before confirmation. Water scenes retain closest
  // boundary traversal so Fresnel loss and absorption stay ordered correctly.
  if ((rayFrame.control.w & 1u) == 0u) {
    Hit hit;
    return traceRayFlags(origin, direction, maximum, seed, gl_RayFlagsTerminateOnFirstHitEXT, false, hit)
               ? vec3(0) : attenuation(medium, maximum);
  }
  vec3 weight = vec3(1);
  for (uint boundary = 0; boundary < 32u; ++boundary) {
    Hit hit;
    bool found = traceRay(origin, direction, maximum, seed, hit);
    weight *= attenuation(medium, hit.distance);
    if (!found)
      return weight;
    RayMaterial material = rayMaterials[rayTriangles[hit.primitive].w];
    if (!dielectric(material))
      return vec3(0);
    vec2 uv;
    vec4 color;
    vec3 normal, geometric;
    attributes(hit.primitive, hit.bary, uv, color, normal, geometric);
    if (thinDielectric(material)) {
      weight *= materialSheet(material, uv, abs(dot(normal, direction)), medium.index).transmission;
    } else {
      bool entering = dot(normal, direction) < 0;
      Medium next = entering ? materialMedium(material) : airMedium();
      weight *= 1 - fresnelBoundary(abs(dot(normal, direction)), medium.index, next.index);
      medium = next;
    }
    float advance = hit.distance + rayFrame.trace.y;
    maximum -= advance;
    if (maximum <= 0)
      return weight;
    origin += direction * advance;
  }
  return vec3(0); // bounded work for coincident or pathological boundaries
}
