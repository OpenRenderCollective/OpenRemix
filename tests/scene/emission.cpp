#include "scene/fixtures.h"
#include "scene/emission.h"
#include "scene/trace_frame.h"
#include "scene/light_bvh.h"
#include <cmath>
#include <limits>
using namespace remix::scene;
using namespace remix::test;
int main() {
  auto response = [](unsigned fps) {
    EmissionHistory h;
    SphereLight l{}; l.id=1; l.triangle=1; l.radiance[0]=1;
    h.begin(1,0,1); h.apply(l,7); h.end();
    for (unsigned f=1; f<=fps; ++f) {
      l.radiance[0]=3; h.begin(1,double(f)/fps,1); h.apply(l,7); h.end();
    }
    return l.radiance[0];
  };
  for (unsigned fps : {30u,60u,144u})
    require(std::abs(response(fps)-(3-2*std::exp(-2.)))<1e-5, "Emission smoothing depends on frame rate");
  EmissionHistory periodic;
  SphereLight animated{}; animated.id=9; animated.triangle=1;
  double average=0;
  for (unsigned f=0; f<=600; ++f) {
    animated.radiance[0]=1+.5f*std::sin(float(f)*2*3.14159265359f/60);
    periodic.begin(1,double(f)/60,1); periodic.apply(animated,1); periodic.end();
    if (f>540) average+=animated.radiance[0]/60;
  }
  require(std::abs(average-1)<1e-4, "Emission smoothing changed periodic mean radiance");
  EmissionHistory history;
  SphereLight source{}; source.id=4; source.triangle=1; source.radiance[0]=2;
  history.begin(1,0,1); history.apply(source,5); history.end();
  require(source.radiance[0]==2, "New emission fades in");
  source.radiance[0]=0; history.begin(1,.01,1); history.apply(source,5); history.end();
  require(source.radiance[0]==0&&history.size()==0, "Disabled emission lingers");
  source.radiance[0]=2; history.begin(1,.02,1); history.apply(source,5); history.end();
  source.radiance[0]=4; source.corners[0][0]=1;
  history.begin(1,.03,1); history.apply(source,5); history.end();
  require(source.radiance[0]==4, "Geometry change retained stale emission");
  source.radiance[0]=6; history.begin(1,.04,1); history.apply(source,6); history.end();
  require(source.radiance[0]==6, "Material/layout change retained stale emission");
  history.begin(1,.05,1); history.end();
  require(history.size()==0, "Absent source history was retained");
  source.radiance[0]=1; history.begin(1,.06,2); history.apply(source,6); history.end();
  source.radiance[0]=8; history.begin(1,.07,2); history.apply(source,6); history.end();
  require(source.radiance[0]==1, "Diagnostic freeze did not hold cast radiance");
  source.radiance[0]=8; history.begin(2,.08,2); history.apply(source,6); history.end();
  require(source.radiance[0]==8, "World change retained old cast radiance");
  Draw draw;
  draw.material = std::make_shared<Material>();
  draw.surface = std::make_shared<Surface>();
  draw.surface->vertices = {{{0,0,0},{0,0},{}},{{1,0,0},{.49f,0},{}},{{0,1,0},{0,1},{}}};
  draw.surface->indices = {0,1,2};
  draw.instanceId = 5;
  auto &m = *draw.material;
  std::fill_n(m.emission,3,2.f);
  m.emissionTexture = std::make_shared<Texture>();
  m.emissionTexture->pixels = {2,1,true,{{128,0,0,255,0,255,0,255}}};
  const auto layout = emissionHistoryKey(draw, 0);
  m.emissionTexture->pixels.mips[0][0] = 64;
  require(emissionHistoryKey(draw, 0) == layout, "Animated texels invalidate emission history");
  m.emissionTexture->pixels.mips[0][0] = 128;
  draw.surface->vertices[0].uv[0] = .1f;
  require(emissionHistoryKey(draw, 0) != layout, "UV layout retained stale emission history");
  draw.surface->vertices[0].uv[0] = 0;
  m.emission[0] = 3;
  require(emissionHistoryKey(draw, 0) != layout, "Emission coefficient retained stale history");
  m.emission[0] = 2;
  const float uv[] = {1,1,0,0};
  auto color = triangleEmission(draw,0,uv);
  require(std::abs(color[0] - .43172f)<.0001f && color[1]==0, "UV region/linear emission average");
  require(triangleEmission(draw,0,uv)==color && m.emissionCache.size()==1, "Emission reduction cache");
  auto light = triangleEmitter(draw,0,color);
  require(light.triangle==1 && std::abs(light.position[0]-1.f/3)<1e-6f, "Area emitter centroid");
  LightBvh bvh;
  bvh.update({light});
  const float expected=.5f*color[0]*.2126f/(4*3.14159265359f);
  require(std::abs(bvh.nodes()[0].minimumPower[3]-expected)<1e-6f, "Area emitter flux");
  draw.model[0]=2; draw.model[5]=3; draw.model[12]=10;
  auto moved = triangleEmitter(draw,0,color);
  require(moved.id==light.id && moved.corners[1][0]==12, "Transform changed emitter identity");
  bvh.update({moved});
  require(bvh.refits==1 && std::abs(bvh.nodes()[0].minimumPower[3]-6*expected)<1e-6f, "Scaled area/refit");
  m.emissionTexture.reset();
  require(triangleEmission(draw,0,uv)[0]==2, "Constant emission fallback");
  remix::NativeResources resources;
  remixapi_MaterialInfoOpaqueEXT opaque{};
  opaque.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
  opaque.albedoConstant={1,1,1}; opaque.opacityConstant=1; opaque.alphaTestType=7;
  remixapi_MaterialInfo info{};
  info.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_INFO; info.pNext=&opaque; info.hash=1;
  info.emissiveIntensity=2; info.emissiveColorConstant={1,0,0};
  remixapi_MaterialHandle handle{};
  resources.createMaterial(info,handle);
  auto mesh=quad(resources,2,handle);
  info.emissiveIntensity=std::numeric_limits<float>::quiet_NaN();
  rejected([&]{resources.createMaterial(info,handle);});
  resources.drawInstance(instance(mesh));
  require(resources.draws()[0].material->emission[0]==2, "Rejected material replaced accepted emission");
  info.emissiveIntensity=2;
  remixapi_MaterialInfoEmissionEXT policy{};
  policy.sType=REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT;
  opaque.pNext=&policy;
  policy.suppressLightGeneration=2;
  rejected([&]{resources.createMaterial(info,handle);});
  auto original=resources.draws()[0].material;
  policy.suppressLightGeneration=1;
  resources.createMaterial(info,handle);
  resources.drawInstance(instance(mesh));
  auto suppressed=resources.draws().back();
  require(suppressed.material!=original && suppressed.material->suppressEmissionLight &&
              suppressed.material->emission[0]==2 && !original->suppressEmissionLight,
          "Emission policy replacement changed visible radiance or queued material");
  SurfaceEmissionCache cache;
  require(prepareTraceFrame({suppressed},0,false,cache).flatEmitters().empty(),
          "Visible-only material generated area lights");
  policy.suppressLightGeneration=0;
  resources.createMaterial(info,handle);
  resources.drawInstance(instance(mesh));
  require(prepareTraceFrame({resources.draws().back()},0,false,cache).flatEmitters().size()==2,
          "Re-enabling emission did not restore area lights");
  opaque.pNext=nullptr;
  resources.createMaterial(info,handle);
  resources.drawInstance(instance(mesh));
  require(!resources.draws().back().material->suppressEmissionLight,
          "Absent emission policy did not preserve default light generation");
  // Exercise every encoded channel, including both sides of the sRGB knee.
  // Uniform texels have an analytic average independent of UV quadrature.
  m.emissionTexture = std::make_shared<Texture>();
  for (bool srgb : {false, true})
    for (unsigned byte = 0; byte < 256; ++byte) {
      m.emissionCache.clear();
      m.emissionTexture->pixels = {1, 1, srgb, {{uint8_t(byte), uint8_t(byte), uint8_t(byte), 255}}};
      const float encoded = byte / 255.f;
      const float linear = !srgb                ? encoded
                           : encoded <= .04045f ? encoded / 12.92f
                                                : std::pow((encoded + .055f) / 1.055f, 2.4f);
      const auto reduced = triangleEmission(draw, 0, uv);
      for (unsigned c = 0; c < 3; ++c)
        require(std::abs(reduced[c] - linear * m.emission[c]) < 1e-5f,
                "Encoded emission channel conversion changed energy");
    }
  std::puts("Emission color, atlas region, transforms, flux, caching and validation passed");
}
