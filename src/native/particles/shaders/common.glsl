// Particle emitters (remix_particles_ext.h): the layout every pass shares.
// particles/renderer.cpp mirrors these structures.
struct Frame {
  mat4 viewProjection;  // the world camera, as the frame's raster overlays
  vec4 camera;          // position, seconds since the previous frame
  vec4 cameraRight;     // world axes of the view; w: seconds since start
  vec4 cameraUp;
  vec4 cameraDelta;     // camera motion since the previous frame
  vec4 worldUp;         // up axis; w: exposure
  vec4 sunDirection;    // toward the sun
  vec4 sunRadiance;     // irradiance at normal incidence; w: 1 when lit by the light volume
  vec4 ambient;         // the sky's mean radiance; w: 1 when the light pass measures it
  vec4 volumeOrigin;    // world position of the volume's cell (0, 0, 0); w: cell size
  ivec4 volumeCell;     // the volume's first cell, in cells; w: the quarter of cells updated
  uvec4 volumeSize;     // cells along x, y, z; w: emitters
  uvec4 display;        // x bit 0: HDR10 (displayColor), bit 1: the environment's texels are sRGB-encoded (gamma 2.2); peak, paper white nits (float bits); reserved
  vec4 depthScale;      // view depth = y / (depth - x); z: 1 when the scene's depth is bound
  vec4 dome[3];         // world to the environment's frame (rows); w: its scale, target width, height
  uvec4 counts;         // frame number, emitters, spawn groups, simulate groups
  vec4 cloudShadow[3];  // world position (w 1) to the clouds' shadow box, (u, v, slice) in [0, 1]
  uvec4 cloudShadowConfig; // 1 with the clouds' shadow, slices
};

struct Emitter {
  vec4 transform[3];    // emitter space to world, rows
  vec4 extent;          // half extents
  uvec4 kind;           // shape, facing, blend, flags
  uvec4 slice;          // offset, capacity, count slot, seed
  uvec4 groups;         // first simulate group, first spawn, spawns, reset
  vec4 velocity;        // world; w: speed
  vec4 axis;            // cone axis; w: cosine of the cone
  vec4 acceleration;    // world; w: drag
  vec4 motion;          // lifetime min, max, turbulence, stretch
  vec4 size;            // start, end, jitter, soft distance
  vec4 colorStart;
  vec4 colorEnd;
  vec4 emission;        // rgb; w: 1 with a texture
};

struct Particle {
  vec4 position;        // xyz; w: age
  vec4 velocity;        // xyz; w: lifetime
  vec4 data;            // size scale, travel left, seed (24 bits), unused
  uvec4 light;          // lit radiance (half floats in xy)
};

#define SHAPE_POINT 0u
#define SHAPE_BOX 1u
#define SHAPE_SPHERE 2u
#define SHAPE_CAMERA_VOLUME 3u
#define FACING_CAMERA 0u
#define FACING_VELOCITY 1u
#define FACING_HORIZONTAL 2u
#define FLAG_FOLLOW_WEATHER 1u
#define FLAG_SHELTER 2u
#define FLAG_CAMERA_SPACE 4u
#define FLAG_UNLIT 8u
#define MAX_EMITTERS 256u
#define NO_HIT 1e30

// The draw stages only read (vertex stages may not write without
// vertexPipelineStoresAndAtomics).
#ifdef DRAW
#define WRITABLE readonly
#else
#define WRITABLE
#endif
layout(set=0,binding=0,std430) readonly buffer Frames { Frame frame; Emitter emitters[]; };
layout(set=0,binding=1,std430) readonly buffer Source { Particle source[]; };
layout(set=0,binding=2,std430) WRITABLE buffer Target { Particle target[]; };
layout(set=0,binding=3,std430) readonly buffer SourceCounts { uint sourceCounts[]; };
layout(set=0,binding=4,std430) WRITABLE buffer TargetCounts { uint targetCounts[]; };
layout(set=0,binding=5,std430) WRITABLE buffer Indirect { uvec4 indirect[]; };
// live, dropped, sheltered, unused
layout(set=0,binding=6,std430) WRITABLE buffer Statistics { uint statistics[4]; };
// The sky's mean radiance (xyz of the first four), then the sun's visibility per cell.
layout(set=0,binding=7,std430) WRITABLE buffer Light { float light[]; };
layout(push_constant) uniform Push { uvec4 push; };

// An integer mix, as pathtrace.comp's.
uint hash(uint x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  return x ^ (x >> 16);
}
float random(inout uint state) {
  state = hash(state);
  return float(state >> 8) * (1.0 / 16777216.0);
}
vec3 randomDirection(inout uint state) {
  float z = random(state) * 2.0 - 1.0, phi = random(state) * 6.28318531;
  float r = sqrt(max(0.0, 1.0 - z * z));
  return vec3(r * cos(phi), r * sin(phi), z);
}
// Uniform in the cone of cosine cosCone around axis.
vec3 coneDirection(vec3 axis, float cosCone, inout uint state) {
  float z = mix(1.0, cosCone, random(state)), phi = random(state) * 6.28318531;
  float r = sqrt(max(0.0, 1.0 - z * z));
  vec3 t = abs(axis.z) < .999 ? normalize(cross(axis, vec3(0, 0, 1))) : vec3(1, 0, 0);
  vec3 b = cross(axis, t);
  return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + axis * z);
}

vec3 emitterPoint(Emitter e, vec3 local) {
  return vec3(dot(e.transform[0], vec4(local, 1)), dot(e.transform[1], vec4(local, 1)),
              dot(e.transform[2], vec4(local, 1)));
}
vec3 emitterDirection(Emitter e, vec3 local) {
  return vec3(dot(e.transform[0].xyz, local), dot(e.transform[1].xyz, local), dot(e.transform[2].xyz, local));
}
// A camera volume's centre: above the camera by a quarter of its height.
vec3 volumeCentre(Emitter e) {
  return frame.camera.xyz + frame.worldUp.xyz * (e.extent.z * .25);
}
bool insideVolume(Emitter e, vec3 p) {
  vec3 d = p - volumeCentre(e);
  vec3 local = vec3(dot(vec3(e.transform[0].x, e.transform[1].x, e.transform[2].x), d),
                    dot(vec3(e.transform[0].y, e.transform[1].y, e.transform[2].y), d),
                    dot(vec3(e.transform[0].z, e.transform[1].z, e.transform[2].z), d));
  return all(lessThanEqual(abs(local), e.extent.xyz));
}

// A world cell's place in the volume's storage (GLSL leaves % of negative
// numbers undefined).
ivec3 wrapCell(ivec3 world, ivec3 size) {
  return world - size * ivec3(floor(vec3(world) / vec3(size)));
}

// The sun's visibility at p from the light volume, trilinear; one outside it
// or without it.
float sunVisibility(vec3 p) {
  if (frame.sunRadiance.w == 0.0) return 1.0;
  ivec3 size = ivec3(frame.volumeSize.xyz);
  vec3 cell = (p - frame.volumeOrigin.xyz) / frame.volumeOrigin.w - .5;
  if (any(lessThan(cell, vec3(0))) || any(greaterThanEqual(cell, vec3(size - 1)))) return 1.0;
  ivec3 base = ivec3(floor(cell));
  vec3 f = cell - vec3(base);
  float sum = 0.0;
  for (int i = 0; i < 8; ++i) {
    ivec3 o = ivec3(i & 1, (i >> 1) & 1, i >> 2);
    // Stored by world cell, wrapped (the volume follows the camera).
    ivec3 wrapped = wrapCell(frame.volumeCell.xyz + base + o, size);
    float v = light[4 + wrapped.x + size.x * (wrapped.y + size.y * wrapped.z)];
    vec3 w = mix(1.0 - f, f, vec3(o));
    sum += v * w.x * w.y * w.z;
  }
  return sum;
}

// What a particle reflects of the sun and the sky, as a white diffuse
// surface: the sky's radiance, and the sun's irradiance over pi, brighter
// toward the sun's side of the particle as the camera sees it.
vec3 particleLight(Emitter e, vec3 p) {
  if ((e.kind.w & FLAG_UNLIT) != 0u) return vec3(1);
  vec3 ambient = frame.ambient.w != 0.0 ? vec3(light[0], light[1], light[2]) : frame.ambient.rgb;
  vec3 toCamera = normalize(frame.camera.xyz - p);
  float phase = mix(.5, 1.0, .5 + .5 * dot(frame.sunDirection.xyz, toCamera));
  return ambient + frame.sunRadiance.rgb * (sunVisibility(p) * phase * .31830989);
}

uvec4 packLight(vec3 radiance) {
  radiance = clamp(radiance, vec3(0), vec3(65000));
  return uvec4(packHalf2x16(radiance.rg), packHalf2x16(vec2(radiance.b, 0)), 0, 0);
}
vec3 unpackLight(uvec4 packed) {
  return vec3(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y).x);
}
