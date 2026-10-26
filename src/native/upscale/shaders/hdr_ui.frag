#version 450
// HDR10's UI brightness (runtime/hdr_output.cpp, HdrOutput::scaleUi): the
// back buffer times the blend constant (blend: zero, constant colour). The
// value written is not read.
layout(location=0) out vec4 result;
void main() {
  result = vec4(0.0);
}
