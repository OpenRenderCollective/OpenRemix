const float Pi = 3.14159265359;
struct BrdfLobes {
  vec3 diffuse;  // BRDF weight; multiply by irradiance or analytic NdotL.
  vec3 specular; // Analytic GGX including NdotL.
};
BrdfLobes brdfLobes(vec3 base, float roughness, float metallic, vec3 n, vec3 v, vec3 l) {
  float nl = max(dot(n, l), 0), nv = max(dot(n, v), 1e-4);
  vec3 halfVector = v + l;
  vec3 f0 = mix(vec3(.04), base, metallic);
  float vh = dot(halfVector, halfVector) > 1e-8 ? max(dot(v, normalize(halfVector)), 0) : 0;
  vec3 f = f0 + (1 - f0) * pow(1 - vh, 5);
  vec3 diffuse = (1 - f) * (1 - metallic) * base / Pi;
  if (nl <= 0 || dot(halfVector, halfVector) < 1e-8)
    return BrdfLobes(diffuse, vec3(0));
  vec3 h = normalize(halfVector);
  float nh = max(dot(n, h), 0);
  float a = max(roughness * roughness, .0025), a2 = a * a;
  float d = a2 / (Pi * pow(nh * nh * (a2 - 1) + 1, 2));
  float visibility = .5 / max(nl * sqrt(nv * nv * (1 - a2) + a2) + nv * sqrt(nl * nl * (1 - a2) + a2), 1e-5);
  return BrdfLobes(diffuse, d * visibility * f * nl);
}
vec3 brdf(vec3 base, float roughness, float metallic, vec3 n, vec3 v, vec3 l) {
  BrdfLobes response = brdfLobes(base, roughness, metallic, n, v, l);
  return response.diffuse * max(dot(n, l), 0) + response.specular;
}
