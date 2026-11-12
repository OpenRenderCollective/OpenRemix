layout(set=1,binding=0,std430) readonly buffer ExposureState { vec4 exposureState; } postState;
layout(set=1,binding=1) uniform sampler2D bloomImage;
// values: exposure, bloom blend, automatic exposure, contrast; lens: x the
// chromatic aberration (fraction of the width at its left and right edges).
layout(set=1,binding=2,std140) uniform PostSettings { vec4 values; vec4 lens; } postSettings;
// Lens chromatic aberration: where the red and blue channels of the pixel at
// `pixel` (of an image `size` pixels) are read, displaced outward and inward
// from the centre in proportion to the distance (none at the centre).
float lensAberration() { return postSettings.lens.x; }
void lensAberrationSamples(vec2 pixel,vec2 size,out vec2 red,out vec2 blue) {
  vec2 offset=(pixel-.5*size)*(2*postSettings.lens.x);
  red=pixel+offset;blue=pixel-offset;
}
vec3 postColor(vec3 color, vec2 uv) {
  if(postSettings.values.y>0)
    color=mix(color,textureLod(bloomImage,uv,0).rgb,postSettings.values.y);
  float automatic=postSettings.values.z>0 ? postState.exposureState.x : 1;
  color*=postSettings.values.x*automatic;
  // Per-channel contrast around middle grey, before the tonemapper (w: the
  // exponent; 0 off).
  if(postSettings.values.w>0)
    color=pow(max(color,vec3(0))/.18,vec3(postSettings.values.w))*.18;
  return color;
}
