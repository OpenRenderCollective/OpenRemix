#version 450
layout(location = 0) flat in uint rejectBackfaces;
void main() {
  if (rejectBackfaces != 0u && !gl_FrontFacing) discard;
}
