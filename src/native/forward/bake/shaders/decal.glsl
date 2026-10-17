// Modulating overlays change the attached receiver's colour, never shadow-ray
// transmission. Projection along the receiver normal also works at grazing views.
vec3 decalModulation(vec3 position, vec3 normal) {
  if (rayFrame.trace.w == 0)
    return vec3(1);
  float reach = rayFrame.trace.z;
  vec3 modulation = vec3(1);
  rayQueryEXT query;
  rayQueryInitializeEXT(query, rayWorld, 0, 255, position + normal * reach, 0,
                         -normal, reach);
  while (rayQueryProceedEXT(query)) {
    if (rayQueryGetIntersectionTypeEXT(query, false) != gl_RayQueryCandidateIntersectionTriangleEXT)
      continue;
    uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
    RayMaterial material = rayMaterials[rayTriangles[primitive].w];
    if ((material.flags.w & 8u) == 0u)
      continue;
    vec2 uv;
    vec4 vertex;
    vec3 n, geometric;
    attributes(primitive, rayQueryGetIntersectionBarycentricsEXT(query, false), uv, vertex, n, geometric);
    if (dot(n, normal) < .99)
      continue;
    vec4 color = baseColor(material, uv, vertex);
    if (alphaPass(material.shading.state.y, uintBitsToFloat(material.shading.state.z), color.a))
      modulation *= max(2 * color.rgb, vec3(0)); // DST_COLOR * source + SRC_COLOR * destination
  }
  return modulation;
}
