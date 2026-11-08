#pragma once
#include <remix/remix_native_ext.h>

// Standalone exports also used to populate the retained SDK interface.
extern "C" {
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Startup(const remixapi_StartupInfo *info);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Shutdown();
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Present(const remixapi_PresentInfo *info);
REMIXAPI remixapi_UIState REMIXAPI_CALL remixapi_GetUIState();
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetUIState(remixapi_UIState state);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetFogState(const remixapi_FogInfo *info);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetScreenTint(float colorR, float colorG, float colorB, float alpha);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterUITexture(remixapi_UITextureHandle id, uint32_t width,
                                                                     uint32_t height, remixapi_Format format,
                                                                     const void *pixelData, uint64_t dataSize);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_FreeUITexture(remixapi_UITextureHandle id);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SubmitUIDrawList(const remixapi_UIDrawList *drawList);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RequestPresentedScreenshot(const char *absolutePath);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetGameValue(const char *key, const char *value);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetGameValue(const char *key, char *outBuffer, uint32_t bufferSize,
                                                                uint32_t *outActualSize);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PumpEvents();
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PollInputEvents(remixapi_InputEvent *events, uint32_t capacity,
                                                                   uint32_t *outCount);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetWindowMetrics(remixapi_WindowMetrics *outMetrics);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetTextInputEnabled(remixapi_Bool enabled);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetPresentationCount(uint64_t *outCount);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetWindowState(remixapi_WindowState *outState);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PollMouseState(remixapi_MouseState *outState);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_IsKeyDown(remixapi_Key key, remixapi_Bool *outIsDown);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetMouseGrabbed(remixapi_Bool grabbed);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetCursorPosition(float x, float y);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetFullscreen(remixapi_Bool fullscreen);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetFeatureAvailability(remixapi_FeatureAvailability *outFeatures);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterCallbacks(PFN_remixapi_BridgeCallback beginSceneCallback,
                                                                     PFN_remixapi_BridgeCallback endSceneCallback,
                                                                     PFN_remixapi_BridgeCallback presentCallback);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_AutoInstancePersistentLights();
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateLightDefinition(remixapi_LightHandle handle,
                                                                         const remixapi_LightInfo *info);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_InitializeLibrary(const remixapi_InitializeLibraryInfo *info,
                                                                     remixapi_Interface *outResult);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetBackendCapabilitiesEXT(remixapi_BackendCapabilitiesEXT *out);
REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetTracingStatisticsEXT(remixapi_TracingStatisticsEXT *out);
}
