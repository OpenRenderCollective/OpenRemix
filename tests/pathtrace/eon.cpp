// Compile the production GLSL evaluator/sampler as C++ with a minimal vector
// adapter, so the furnace and PDF checks exercise the actual shader equations.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using std::clamp;
using std::cos;
using std::max;
using std::pow;
using std::sin;
using std::sqrt;
struct vec4;
struct vec3 {
  float x, y, z;
  vec3() = default;
  explicit vec3(float v) : x(v), y(v), z(v) {}
  vec3(float a, float b, float c) : x(a), y(b), z(c) {}
  explicit vec3(const vec4 &v);
};
struct vec4 {
  float x, y, z, w;
  vec4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
  vec4(vec3 v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
};
vec3::vec3(const vec4 &v) : x(v.x), y(v.y), z(v.z) {}
vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
vec3 operator*(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
vec3 operator/(vec3 a, vec3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
vec3 operator*(vec3 a, float b) { return {a.x * b, a.y * b, a.z * b}; }
vec3 operator/(vec3 a, float b) { return a * (1 / b); }
float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float inversesqrt(float v) { return 1 / sqrt(v); }
vec3 normalize(vec3 a) { return a * inversesqrt(dot(a, a)); }
float mix(float a, float b, float t) { return a * (1 - t) + b * t; }
struct mat3 {
  vec3 x, y, z;
  mat3(vec3 a, vec3 b, vec3 c) : x(a), y(b), z(c) {}
};
vec3 operator*(mat3 m, vec3 v) { return m.x * v.x + m.y * v.y + m.z * v.z; }
mat3 transpose(mat3 m) { return {{m.x.x, m.y.x, m.z.x}, {m.x.y, m.y.y, m.z.y}, {m.x.z, m.y.z, m.z.z}}; }
#include "pathtrace/shaders/eon.glsl"
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
int main() {
  // A tilted world frame must give the same isotropic response as local +Z,
  // including grazing views and opposing azimuths. The production NEE path
  // uses world dot products; the reference rotates both directions explicitly.
  const mat3 rotated({.6f, 0, -.8f}, {0, 1, 0}, {.8f, 0, .6f});
  for (float r : {0.f, .01f, .25f, .5f, 1.f})
    for (float ni : {.001f, .05f, .5f, 1.f})
      for (float no : {.001f, .05f, .5f, 1.f})
        for (float phi : {0.f, .5f, 1.5f, 3.14159265f, 4.5f}) {
          const vec3 wi(sqrt(1 - ni * ni) * cos(phi), sqrt(1 - ni * ni) * sin(phi), ni);
          const vec3 wo(sqrt(1 - no * no), 0, no), rho(.1f, .5f, .9f);
          const vec3 worldI = rotated * wi, worldO = rotated * wo;
          const vec3 reference = eonBrdf(rho, r, wi, wo);
          const vec3 actual =
              eonBrdfCosines(rho, r, dot(rotated.z, worldI), dot(rotated.z, worldO), dot(worldI, worldO));
          require(dot(actual - reference, actual - reference) < 1e-9f + 1e-8f * dot(reference, reference),
                  "EON world-frame evaluation changed reflectance");
        }
  for (float roughness : {0.f, .01f, .25f, .5f, 1.f})
    for (float mu : {1.f, .5f, .05f, .001f}) {
      vec3 wo(sqrt(1 - mu * mu), 0, mu);
      double furnace = 0, pdf = 0, sampled = 0;
      constexpr int n = 256;
      for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
          float z = (i + .5f) / n, phi = 2 * eonPi * (j + .5f) / n;
          vec3 wi(sqrt(1 - z * z) * cos(phi), sqrt(1 - z * z) * sin(phi), z);
          vec3 f = eonBrdf(vec3(1), roughness, wi, wo);
          furnace += f.x * z * 2 * eonPi / (n * n);
          pdf += eonPdf(wo, wi, roughness) * 2 * eonPi / (n * n);
          vec3 rho(.1f, .5f, .9f);
          vec3 a = eonBrdf(rho, roughness, wi, wo), b = eonBrdf(rho, roughness, wo, wi);
          require(dot(a - b, a - b) < 1e-8f, "EON reciprocity");
          require(a.x >= 0 && a.y >= 0 && a.z >= 0, "EON negative reflectance");
          if (roughness == 0)
            require(dot(a - rho / eonPi, a - rho / eonPi) < 1e-12f, "Lambert limit");
          vec4 draw = eonSample(wo, roughness, z, (j + .5f) / n);
          vec3 direction(draw);
          require(std::isfinite(draw.w) && draw.w > 0 && draw.z >= 0 && std::abs(dot(direction, direction) - 1) < 1e-5f,
                  "Invalid EON sample");
          sampled += eonBrdf(vec3(1), roughness, direction, wo).x * draw.z / draw.w / (n * n);
        }
      require(std::abs(furnace - 1) < .006, "EON white furnace lost energy");
      require(std::abs(pdf - 1) < .006, "EON PDF not normalized");
      require(std::abs(sampled - 1) < .006, "EON sample disagrees with furnace");
      for (float u : {0.f, 1.f}) {
        auto draw = eonSample(wo, roughness, u, 0);
        require(std::isfinite(draw.x) && std::isfinite(draw.z) && std::isfinite(draw.w), "EON endpoint produced NaN");
      }
      std::printf("r %.2f mu %.3f furnace %.5f PDF %.5f estimate %.5f\n", roughness, mu, furnace, pdf, sampled);
    }
}
