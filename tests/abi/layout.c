#include <remix/remix_native_ext.h>
#include <remix/remix_editor_ext.h>
#include <remix/remix_usd_ext.h>
#include <remix/remix_particles_ext.h>
#if defined(_WIN32)
#include "windows_layout.h"
#else
#include "posix_layout.h"
#endif
_Static_assert(REMIXAPI_VERSION_MINOR == 1003, "ABI line");
_Static_assert(sizeof(remixapi_MaterialInfoEmissionEXT) == 24, "Material emission extension ABI");
_Static_assert(offsetof(remixapi_MaterialInfoEmissionEXT, suppressLightGeneration) == 16,
               "Material emission policy alignment");
_Static_assert(sizeof(remixapi_CameraEnvironmentEXT) == 32, "Camera environment ABI");
_Static_assert(offsetof(remixapi_CameraEnvironmentEXT, enabled) == 16, "Camera environment enabled");
_Static_assert(offsetof(remixapi_CameraEnvironmentEXT, radiance) == 20, "Camera environment radiance");
_Static_assert(sizeof(remixapi_SharcStatisticsEXT) == 344, "SHARC statistics ABI");
_Static_assert(offsetof(remixapi_SharcStatisticsEXT, memoryBytes) == 48, "SHARC statistics alignment");
_Static_assert(sizeof(remixapi_ShadowStatisticsEXT) == 312, "Shadow statistics ABI");
_Static_assert(offsetof(remixapi_ShadowStatisticsEXT, renderedFrames) == 32, "Shadow statistics alignment");
_Static_assert(sizeof(remixapi_LocalLightStatisticsEXT) == 344, "Local light statistics ABI");
_Static_assert(sizeof(remixapi_LocalLightGuidingStatisticsEXT) == 320, "Local guiding statistics ABI");
_Static_assert(sizeof(remixapi_LocalLightSetStatisticsEXT) == 184, "Local light set statistics ABI");
_Static_assert(offsetof(remixapi_LocalLightSetStatisticsEXT, builds) == 32, "Local light set statistics alignment");
_Static_assert(offsetof(remixapi_LocalLightGuidingStatisticsEXT, renderedFrames) == 32, "Local guiding alignment");
_Static_assert(offsetof(remixapi_LocalLightStatisticsEXT, generation) == 40, "Local light statistics alignment");
_Static_assert(offsetof(remixapi_EditorSharedFrame, device_uuid) == 24, "Device UUID offset");
#if defined(_WIN32)
_Static_assert(sizeof(remixapi_EditorSharedFrame) == 64, "Shared frame version 1 ABI");
_Static_assert(offsetof(remixapi_EditorSharedFrame, memory_handle) == 40, "Shared memory handle offset");
#else
_Static_assert(sizeof(remixapi_EditorSharedFrame) == 56, "Shared frame version 1 ABI");
_Static_assert(offsetof(remixapi_EditorSharedFrame, memory_fd) == 40, "Shared memory FD offset");
#endif
_Static_assert(sizeof(remixapi_EditorStartupEXT) == 32, "Editor startup extension ABI");
_Static_assert(sizeof(remixapi_MaterialNormalEncodingEXT) == 24, "Normal encoding ABI");
_Static_assert(sizeof(remixapi_MaterialPbrChannelEXT) == 24, "PBR channel ABI");
_Static_assert(sizeof(remixapi_MaterialPbrRemapEXT) == 64, "PBR remap ABI");
_Static_assert(sizeof(remixapi_MaterialNormalScrollEXT) == 48, "Normal scroll ABI");
_Static_assert(sizeof(remixapi_MaterialIridescenceEXT) == 64, "Iridescence ABI");
_Static_assert(sizeof(remixapi_CameraMediumEXT) == 40, "Camera medium ABI");
_Static_assert(sizeof(remixapi_USDLoadOptionsEXT) == 32, "USD load version 1 ABI");
_Static_assert(sizeof(remixapi_USDDrawOptionsEXT) == 16, "USD draw version 1 ABI");
_Static_assert(sizeof(remixapi_USDDiagnosticsEXT) == 1088, "USD diagnostics version 1 ABI");
_Static_assert(sizeof(remixapi_HostShaderBindingEXT) == 8, "Host shader binding ABI");
_Static_assert(sizeof(remixapi_HostShaderInfoEXT) == 56, "Host shader info ABI");
_Static_assert(sizeof(remixapi_HostPassBufferEXT) == 24, "Host pass buffer ABI");
_Static_assert(sizeof(remixapi_HostPassSamplerEXT) == 32, "Host pass sampler ABI");
_Static_assert(sizeof(remixapi_HostPassVertexBindingEXT) == 16, "Host pass vertex binding ABI");
_Static_assert(sizeof(remixapi_HostPassAttributeEXT) == 16, "Host pass attribute ABI");
_Static_assert(sizeof(remixapi_HostPassEXT) == 272, "Host pass ABI");
_Static_assert(sizeof(remixapi_HostPassListEXT) == 32, "Host pass list ABI");
_Static_assert(sizeof(remixapi_HostPassStatisticsEXT) == 72, "Host pass statistics ABI");
_Static_assert(sizeof(remixapi_InstanceInfoWireframeEXT) == 24, "Wireframe instance ABI");
_Static_assert(sizeof(remixapi_ParticleEmitterInfoEXT) == 232, "Particle emitter ABI");
_Static_assert(offsetof(remixapi_ParticleEmitterInfoEXT, transform) == 40, "Particle emitter transform");
_Static_assert(offsetof(remixapi_ParticleEmitterInfoEXT, colorStart) == 168, "Particle emitter colours");
_Static_assert(offsetof(remixapi_ParticleEmitterInfoEXT, texture) == 216, "Particle emitter texture");
_Static_assert(sizeof(remixapi_ParticleStatisticsEXT) == 56, "Particle statistics ABI");
int main(void) { return 0; }
