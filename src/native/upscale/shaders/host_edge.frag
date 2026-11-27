#version 450
// Host pass edge smoothing (runtime/host_passes.cpp), second stage. The
// host's depth-tested draws are tested against one depth sample per pixel
// while the frame under them is anti-aliased: where they meet the scene (and
// at their own silhouettes) the boundary is a staircase. The boundary of the
// pixels the draws changed (`mask`) is taken for the staircase of a straight
// line, as morphological anti-aliasing does: along each run of boundary
// between two steps the line goes from half a pixel on one side to half a
// pixel on the other (or back to the boundary at the run's middle, where both
// ends step the same way), and the part of a pixel that line cuts off changes
// sides. A changed pixel gives that part back to the frame as it was
// (`before`); an unchanged one takes it from the changed neighbour across the
// boundary. Pixels off the boundary are copied.
layout(binding = 0) uniform sampler2D mask; // linear, clamped: a tap between two rows averages them
layout(binding = 1) uniform sampler2D before;
layout(binding = 2) uniform sampler2D after;
layout(location = 0) out vec4 color;

// How far a run of boundary is followed each way. A step farther than this
// from a pixel leaves it as it is, so slopes under 1 in 2 * kReach keep a
// short ramp at each step rather than one along the whole run.
const int kReach = 32;

// Follows the boundary between a changed row and the unchanged row beside it
// from `start` (the boundary's middle at this pixel). `steps`: the pixels the
// run goes on for. The result is the line's height at that end, toward the
// unchanged row: -0.5 where the changed row ends (the boundary steps back
// into it), +0.5 where the unchanged row ends, 0 where the run outlasts the
// search.
float follow(vec2 start, vec2 step, out float steps) {
  for (int k = 1; k <= kReach; ++k) {
    const float both = textureLod(mask, start + step * float(k), 0).r;
    if (both < 0.25) {
      steps = float(k - 1);
      return -0.5;
    }
    if (both > 0.75) {
      steps = float(k - 1);
      return 0.5;
    }
  }
  steps = float(kReach);
  return 0;
}

// The line's height `x` pixels along a run `run` pixels long.
float height(float x, float run, float left, float right) {
  return x < run * 0.5 ? left * (1 - 2 * x / run) : right * (2 * x / run - 1);
}

void main() {
  const ivec2 p = ivec2(gl_FragCoord.xy);
  const ivec2 last = textureSize(mask, 0) - 1;
  const vec2 texel = 1.0 / vec2(textureSize(mask, 0));
  const bool changed = texelFetch(mask, p, 0).r > 0.5;
  const vec4 here = texelFetch(after, p, 0);
  const ivec2 sides[4] = ivec2[4](ivec2(0, -1), ivec2(0, 1), ivec2(-1, 0), ivec2(1, 0));
  float given = 0, taken = 0;
  vec4 takenColor = vec4(0);
  for (int s = 0; s < 4; ++s) {
    const ivec2 n = p + sides[s];
    if (any(lessThan(n, ivec2(0))) || any(greaterThan(n, last)))
      continue;
    if ((texelFetch(mask, n, 0).r > 0.5) == changed)
      continue;
    const vec2 across = vec2(sides[s]), along = abs(across.yx);
    const vec2 start = (vec2(p) + 0.5 + 0.5 * across) * texel;
    float left, right;
    const float leftEnd = follow(start, -along * texel, left), rightEnd = follow(start, along * texel, right);
    const float run = left + right + 1;
    // The line is straight over each half of the pixel and keeps its sign there.
    const float h1 = height(left + 0.25, run, leftEnd, rightEnd), h2 = height(left + 0.75, run, leftEnd, rightEnd);
    if (changed) {
      given += 0.5 * (max(-h1, 0) + max(-h2, 0));
    } else {
      float part = 0.5 * (max(h1, 0) + max(h2, 0));
      if (part > 0) {
        // Scaled down where the neighbour barely changed: a faint effect's
        // boundary (a glow fading to nothing) needs no smoothing, and taking
        // the neighbour there would only blur the frame under it.
        const vec4 there = texelFetch(after, n, 0);
        const vec3 change = abs(there.rgb - texelFetch(before, n, 0).rgb);
        part *= clamp(max(change.r, max(change.g, change.b)) * 16, 0, 1);
        takenColor += there * part;
        taken += part;
      }
    }
  }
  if (changed) {
    color = given > 0 ? mix(here, texelFetch(before, p, 0), min(given, 1)) : here;
  } else {
    if (taken > 1) {
      takenColor /= taken;
      taken = 1;
    }
    color = here * (1 - taken) + takenColor;
  }
}
