// Default reflection-source wiring. Alternative providers implement the same
// sampleEnvironmentReflection(ReflectionReceiver) contract at shader compilation.
// World resolve also calls environmentReflectionCandidates(position, valid)
// uniformly before early returns; a provider without candidates returns zero.
#include "reflection-values.glsl"
#include "reflection-environment.glsl"
