// Radiance cascade diffuse implementation (forward/cascades). Selected at shader
// compilation for world receivers and viewmodels; water keeps its own path.
// Reads the gather (irradiance / pi, view depth) through cascadeCamera: this
// frame's camera, or the previous one's when the gather trails shading. Taps on
// another surface (by view depth) are rejected.
layout(set = 0, binding = 23) uniform sampler2D cascadeIrradiance;
#ifdef VIEWMODEL_PASS
// Viewmodels use another camera and are not in the gather. Use the world's
// irradiance around the same screen position: the surroundings just behind.
bool cascadeDiffuse(vec3 position, out vec3 diffuse) {
  diffuse = vec3(0);
  if (frame.cascadeControl.x < .5) return false;
  ivec2 size = textureSize(cascadeIrradiance, 0);
  ivec2 pixel = ivec2(gl_FragCoord.xy * vec2(size) / vec2(frame.target.xy));
  float total = 0;
  for (int y = -2; y <= 2; ++y)
    for (int x = -2; x <= 2; ++x) {
      vec4 value = texelFetch(cascadeIrradiance, clamp(pixel + ivec2(x, y) * 8, ivec2(0), size - 1), 0);
      if (value.a <= 0) continue;
      diffuse += value.rgb;
      total += 1;
    }
  if (total < 1) return false;
  diffuse *= frame.cascadeControl.y / total;
  return true;
}
#else
bool cascadeDiffuse(vec3 position, out vec3 diffuse) {
  diffuse = vec3(0);
  if (frame.cascadeControl.x < .5) return false;
  vec4 clip = frame.cascadeCamera * vec4(position, 1);
  if (abs(clip.w) < 1e-6) return false;
  vec3 ndc = clip.xyz / clip.w;
  if (any(greaterThan(abs(ndc.xy), vec2(1))) || ndc.z < 0 || ndc.z > 1) return false;
  ivec2 size = textureSize(cascadeIrradiance, 0);
  vec2 texel = (ndc.xy * .5 + .5) * vec2(size) - .5;
  ivec2 base = ivec2(floor(texel));
  vec2 t = texel - vec2(base);
  float depth = abs(clip.w), total = 0;
  for (int i = 0; i < 4; ++i) {
    ivec2 offset = ivec2(i & 1, i >> 1);
    vec4 value = texelFetch(cascadeIrradiance, clamp(base + offset, ivec2(0), size - 1), 0);
    float w = mix(1 - t.x, t.x, float(offset.x)) * mix(1 - t.y, t.y, float(offset.y));
    if (value.a <= 0 || abs(value.a - depth) > frame.cascadeControl.z * depth) continue;
    diffuse += w * value.rgb;
    total += w;
  }
  if (total >= 1e-3) {
    diffuse *= frame.cascadeControl.y / total;
    return true;
  }
  // Disoccluded by camera motion: the nearest-depth texel around it within a
  // looser tolerance, rather than dropping to the much darker ambient fallback.
  float best = 3 * frame.cascadeControl.z;
  for (int y = -3; y <= 3; ++y)
    for (int x = -3; x <= 3; ++x) {
      vec4 value = texelFetch(cascadeIrradiance, clamp(base + ivec2(x, y) * 2, ivec2(0), size - 1), 0);
      float difference = abs(value.a - depth) / depth;
      if (value.a > 0 && difference < best) {
        best = difference;
        diffuse = value.rgb;
      }
    }
  diffuse *= frame.cascadeControl.y;
  return best < 3 * frame.cascadeControl.z;
}
// The gather is irradiance about the depth-derived (geometric) normal. Cascade-0
// probes also keep their luminance as L0/L1 spherical harmonics: with radiance
// a + b.w (b = 3 xyz), irradiance / pi about a normal m is a + 2 xyz.m. Rescale by
// the ratio between the shading and geometric normals, so normal maps respond.
layout(set = 0, binding = 24) uniform sampler2D cascadeDirection;
float cascadeNormalResponse(vec3 position, vec3 normal, vec3 geometric) {
  geometric = dot(geometric, geometric) > 1e-12 ? normalize(geometric) : normal;
  if (dot(geometric, normal) < 0) geometric = -geometric;
  if (dot(normal, geometric) > .9999) return 1;
  vec4 clip = frame.cascadeCamera * vec4(position, 1);
  if (abs(clip.w) < 1e-6) return 1;
  // Probe centres sit at spacing * (q + 1/2) pixels: normalised probe coordinates.
  vec2 pixel = (clip.xy / clip.w * .5 + .5) * vec2(textureSize(cascadeIrradiance, 0));
  vec4 sh = textureLod(cascadeDirection, pixel / (frame.cascadeControl.w * vec2(textureSize(cascadeDirection, 0))), 0);
  float shading = sh.w + 2 * dot(sh.xyz, normal), surface = sh.w + 2 * dot(sh.xyz, geometric);
  if (!(surface > 1e-4)) return 1;
  return clamp(max(shading, 0) / surface, .25, 2);
}
#endif
// Incoming-only query shared with other live providers. The cascade source
// owns its unavailable-gather fallback; valid black samples remain black.
vec3 sampleIncomingDiffuse(vec3 position, vec3 normal, vec3 geometric) {
  vec3 incoming = frame.ambientExposure.rgb;
  vec3 cascade;
  if (cascadeDiffuse(position, cascade)) {
#ifndef VIEWMODEL_PASS
    cascade *= cascadeNormalResponse(position, normal, geometric);
#endif
    incoming = cascade;
  }
  return incoming;
}
