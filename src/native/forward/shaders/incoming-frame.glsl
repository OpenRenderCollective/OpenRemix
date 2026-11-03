// Optional provider-owned std140 members after the fixed common frame prefix.
// A replacement incoming source supplies this declaration fragment alongside
// its queries, or leaves it empty when it needs no extra frame values.
#ifdef CASCADE_GI
#include "../cascades/shaders/receiver-frame.glsl"
#endif
