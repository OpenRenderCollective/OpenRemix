#version 450
// A particle's billboard: six vertices per instance, the instance's particle
// from the emitter's slice (push.x: the emitter). The colour is the lit
// radiance, exposed and tonemapped as the composed frame.
#extension GL_GOOGLE_include_directive : require
#define DRAW 1
#include "common.glsl"
#include "../../pathtrace/shaders/display.glsl"
layout(location=0) out vec4 color;          // display colour; alpha
layout(location=1) out vec2 corner;         // -1..1 across the billboard
layout(location=2) flat out uvec2 shape;    // facing, textured
layout(location=3) flat out float softScale; // 1 / soft distance (0: hard)

void main() {
  Emitter e = emitters[push.x];
  Particle p = target[e.slice.x + uint(gl_InstanceIndex)];
  float t = clamp(p.position.w / max(p.velocity.w, 1e-6), 0.0, 1.0);
  float size = mix(e.size.x, e.size.y, t) * p.data.x;
  vec4 c = mix(e.colorStart, e.colorEnd, t);
  vec3 radiance = c.rgb * unpackLight(p.light) + e.emission.rgb;
  color = vec4(displayColor(radiance * frame.worldUp.w, frame.display.x & 1u, uintBitsToFloat(frame.display.y),
                            uintBitsToFloat(frame.display.z)),
               c.a);
  const vec2 corners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
  vec2 q = corners[gl_VertexIndex % 6];
  corner = q;
  vec3 centre = p.position.xyz, right = vec3(0), up = vec3(0);
  uint facing = e.kind.y;
  if (facing == FACING_VELOCITY) {
    // Across the motion as the camera sees it, the streak trailing behind.
    vec3 v = p.velocity.xyz, toCamera = frame.camera.xyz - centre;
    float speed = length(v);
    vec3 side = cross(v, toCamera);
    if (speed > 1e-6 && length(side) > 1e-3 * speed * length(toCamera)) {
      vec3 direction = v / speed;
      float trail = speed * e.motion.w;
      centre -= direction * (trail * .5);
      right = normalize(side) * (size * .5);
      up = direction * ((size + trail) * .5);
    } else {
      facing = FACING_CAMERA;
    }
  }
  if (facing == FACING_CAMERA) {
    right = frame.cameraRight.xyz * (size * .5);
    up = frame.cameraUp.xyz * (size * .5);
  } else if (facing == FACING_HORIZONTAL) {
    vec3 n = frame.worldUp.xyz;
    vec3 a = abs(n.z) < .999 ? normalize(cross(n, vec3(0, 0, 1))) : vec3(1, 0, 0);
    right = a * (size * .5);
    up = cross(n, a) * (size * .5);
  }
  // At least a pixel and a half across: thinner particles widen and fade in
  // proportion, so distant ones keep their coverage instead of flickering.
  vec4 middle = frame.viewProjection * vec4(centre, 1.0), side = frame.viewProjection * vec4(centre + right, 1.0);
  if (middle.w > 0.0 && side.w > 0.0) {
    vec2 viewport = vec2(frame.dome[1].w, frame.dome[2].w) * .5;
    float halfWidth = length((side.xy / side.w - middle.xy / middle.w) * viewport);
    if (halfWidth > 0.0 && halfWidth < .75) {
      float widen = .75 / halfWidth;
      right *= widen;
      color.a /= widen;
      if (facing != FACING_VELOCITY) {
        up *= widen;
        color.a /= widen;
      }
    }
  }
  gl_Position = frame.viewProjection * vec4(centre + right * q.x + up * q.y, 1.0);
  shape = uvec2(facing, e.emission.w != 0.0 ? 1u : 0u);
  softScale = e.size.w > 0.0 ? 1.0 / e.size.w : 0.0;
}
