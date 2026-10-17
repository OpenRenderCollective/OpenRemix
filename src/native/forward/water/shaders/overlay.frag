#version 450
layout(set = 2, binding = 7) uniform sampler2D opaqueSpecular;
layout(location = 0) out vec4 result;
void main() { result = vec4(texelFetch(opaqueSpecular, ivec2(gl_FragCoord.xy), 0).rgb, 0); }
