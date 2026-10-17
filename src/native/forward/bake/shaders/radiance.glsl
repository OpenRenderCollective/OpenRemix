#include "../../shaders/emitter.glsl"
float random(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return float((word >> 22u) ^ word) * (1.0 / 4294967296.0);
}
vec3 basisDirection(vec3 n, float cosine, float phi) {
  vec3 t = normalize(cross(abs(n.z) < .99 ? vec3(0, 0, 1) : vec3(1, 0, 0), n));
  return n * cosine + sqrt(max(0, 1 - cosine * cosine)) * (t * cos(phi) + cross(n, t) * sin(phi));
}
struct Hit {
  float distance;
  uint primitive;
  vec2 bary;
  bool front;
};
bool traceRay(vec3 origin, vec3 direction, float maximum, inout uint seed, out Hit hit) {
  // One stream step per ray keeps repeated samples independent, including rays
  // that miss. Candidate visits must never change the path's random stream.
  uint coverageSeed = seed;
  random(seed);
  rayQueryEXT query;
  rayQueryInitializeEXT(query, rayWorld, 0, 255, origin, rayFrame.trace.y, direction, maximum);
  while (rayQueryProceedEXT(query)) {
    if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
        covered(rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
                rayQueryGetIntersectionBarycentricsEXT(query, false),
                rayQueryGetIntersectionFrontFaceEXT(query, false), coverageSeed))
      rayQueryConfirmIntersectionEXT(query);
  }
  hit = Hit(maximum, 0u, vec2(0), true);
  if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
    return false;
  hit.distance = rayQueryGetIntersectionTEXT(query, true);
  hit.primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
  hit.bary = rayQueryGetIntersectionBarycentricsEXT(query, true);
  hit.front = rayQueryGetIntersectionFrontFaceEXT(query, true);
  return true;
}
#include "optics.glsl"
vec3 environment(vec3 direction) {
  vec3 d = normalize((rayFrame.dome * vec4(direction, 0)).xyz);
  vec2 uv = vec2(atan(d.z, d.x) / (2 * RayPi) + .5, acos(clamp(d.y, -1, 1)) / RayPi);
  return rayFrame.domeRadiance.rgb * sampleMap(rayFrame.control.x, uv, uvec3(1, 3, 1)).rgb;
}
bool sampleEmitter(RayLight light, vec3 p, inout uint seed, out vec3 direction, out float distance,
                   out float weight) {
  if (light.radiance.w > 0) {
    float root = sqrt(random(seed)), v = random(seed);
    vec3 point = light.corners[0].xyz * (1 - root) + light.corners[1].xyz * (root * (1 - v)) +
                 light.corners[2].xyz * (root * v);
    vec3 delta = point - p, normal = cross(light.corners[1].xyz - light.corners[0].xyz,
                                           light.corners[2].xyz - light.corners[0].xyz);
    distance = length(delta);
    direction = delta / max(distance, 1e-8);
    float area = .5 * length(normal);
    float cosine = dot(normal / max(2 * area, 1e-8), -direction);
    if (light.radiance.w > 1)
      cosine = abs(cosine);
    weight = area * max(cosine, 0) / max(distance * distance, 1e-8);
  } else {
    vec3 delta = light.positionRadius.xyz - p;
    float d2 = dot(delta, delta), radius = light.positionRadius.w;
    if (d2 < 1e-12 || radius <= 0)
      return false;
    float oneMinusCos = sphereOneMinusCos(d2, radius);
    vec3 axis = delta / sqrt(d2);
    float offset = random(seed) * oneMinusCos, phi = 2 * RayPi * random(seed);
    vec3 tangent = normalize(cross(abs(axis.z) < .99 ? vec3(0, 0, 1) : vec3(1, 0, 0), axis));
    direction = normalize(axis * (1 - offset) + sqrt(max(0, offset * (2 - offset))) *
                            (tangent * cos(phi) + cross(axis, tangent) * sin(phi)));
    float projected = dot(delta, direction);
    vec3 perpendicular = delta - direction * projected;
    float chord = sqrt(max(0, radius * radius - dot(perpendicular, perpendicular)));
    distance = d2 <= radius * radius ? projected + chord : projected - chord;
    weight = 2 * RayPi * oneMinusCos;
  }
  weight *= emitterConeWeight(light.directionCone.xyz, light.directionCone.w,
                               light.shaping.x, light.shaping.y, -direction);
  return weight > 0;
}
vec3 directLight(vec3 p, vec3 n, inout uint seed, Medium medium) {
  vec3 irradiance = vec3(0);
  if (any(greaterThan(rayFrame.sunRadiance.rgb, vec3(0)))) {
    vec3 direction = normalize(-rayFrame.sunDirection.xyz);
    direction = basisDirection(direction, mix(cos(rayFrame.sunDirection.w), 1, random(seed)),
                               2 * RayPi * random(seed));
    float cosine = max(dot(n, direction), 0);
    if (cosine > 0)
      irradiance += rayFrame.sunRadiance.rgb * cosine * visibility(p, direction, rayFrame.trace.x, seed, medium);
  }
  if (rayFrame.counts.y == 0u)
    return irradiance;
  uint index = min(uint(random(seed) * float(rayFrame.counts.y)), rayFrame.counts.y - 1u);
  vec3 direction;
  float distance, weight;
  if (sampleEmitter(rayLights[index], p, seed, direction, distance, weight)) {
    float cosine = max(dot(n, direction), 0);
    if (cosine > 0)
      irradiance += rayLights[index].radiance.rgb * (weight * cosine * float(rayFrame.counts.y)) *
                    visibility(p, direction, distance - rayFrame.trace.y, seed, medium);
  }
  return irradiance;
}
vec3 radiance(vec3 origin, vec3 direction, Medium medium, inout uint seed, out float firstDistance,
              out bool firstBack, out bool firstMiss) {
  vec3 result = vec3(0), throughput = vec3(1);
  firstDistance = rayFrame.trace.x;
  firstBack = false;
  firstMiss = false;
  uint bounce = 0;
  for (uint step = 0; bounce < rayFrame.counts.w && step < rayFrame.counts.w + 32u; ++step) {
    Hit hit;
    bool found = traceRay(origin, direction, rayFrame.trace.x, seed, hit);
    if (step == 0u) {
      firstDistance = hit.distance;
      firstMiss = !found;
      firstBack = found && !hit.front;
    }
    throughput *= attenuation(medium, hit.distance);
    if (!found) {
      result += throughput * environment(direction);
      break;
    }
    RayMaterial m = rayMaterials[rayTriangles[hit.primitive].w];
    vec2 uv;
    vec4 vertex;
    vec3 n, geometric;
    attributes(hit.primitive, hit.bary, uv, vertex, n, geometric);
    if (dielectric(m)) {
      if (step == 0u)
        firstBack = false; // a water exit is not an invalid solid probe
      bool entering = dot(n, direction) < 0;
      n = entering ? n : -n;
      Medium next = entering ? materialMedium(m) : airMedium();
      float reflectance = fresnelBoundary(-dot(n, direction), medium.index, next.index);
      bool reflectBoundary = reflectance >= 1 || random(seed) < reflectance;
      origin += direction * hit.distance;
      if (reflectBoundary) {
        direction = reflect(direction, n);
        origin += n * rayFrame.trace.y;
      } else {
        float eta = medium.index / next.index;
        direction = normalize(refract(direction, n, eta));
        throughput *= eta * eta; // radiance transport across an index change
        medium = next;
        origin -= n * rayFrame.trace.y;
      }
      continue; // interfaces do not consume the diffuse bounce budget
    }
    if (step == 0u)
      firstBack = dot(n, direction) > 0;
    if (dot(geometric, direction) > 0)
      geometric = -geometric;
    if (dot(n, direction) > 0)
      n = -n;
    if (dot(n, geometric) < .01)
      n = geometric;
    vec4 base = baseColor(m, uv, vertex);
    vec4 emitted = sampleMap(m.textures.y, uv, m.flags.xyz);
    vec3 emission;
    float reflected;
    emissionLobes(m.shading.emission, m.shading.state, base, sampleMap(m.textures.x, uv, m.flags.xyz),
                  emitted, emission, reflected);
    result += throughput * emission;
    float metallic = m.textures.z != 0u ? mapValue(sampleMap(m.textures.z, uv, m.flags.xyz),
                                                   m.shading.remap[1], m.shading.maps.w)
                                        : m.shading.pbr.y;
    vec3 diffuse = clamp(base.rgb, 0, 1) * (reflected * (1 - metallic));
    origin += direction * hit.distance;
    origin += geometric * rayFrame.trace.y;
    result += throughput * diffuse / RayPi * directLight(origin, n, seed, medium);
    throughput *= diffuse;
    if (max(max(throughput.x, throughput.y), throughput.z) < 1e-8)
      break;
    if (bounce > 0u) {
      float survival = clamp(max(max(throughput.x, throughput.y), throughput.z), .05, .95);
      if (random(seed) > survival)
        break;
      throughput /= survival;
    }
    direction = basisDirection(n, sqrt(random(seed)), 2 * RayPi * random(seed));
    ++bounce;
  }
  return result;
}
