#version 450
// The NUMOS sky over the target (raster/sky.cpp): the transmittance image
// for the multiplying draw, the addition image for the adding one.
layout(set=0,binding=0) uniform sampler2D image;
layout(location=0) out vec4 result;
void main() {
  result = vec4(texelFetch(image, ivec2(gl_FragCoord.xy), 0).rgb, 1.0);
}
