// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include "../host/hle.h"
#include <stdatomic.h>

static struct { uint32_t magic; } g_display = {0x50534944}, g_layer = {0x5259414c};
// The native window's magic doubles as the winsys backend's window-type tag: the SDK's "null"
// NvWsi backend (which the host steers EGL surfaces to, see hle_nvdrv.c) validates it as 0x84539653
// and reads the swapchain's pixel size straight out of +4/+8 when it allocates its buffers.
static struct { uint32_t magic; uint32_t w, h; void *layer; } g_native = {0x84539653, 1280, 720, 0};

HLE_FORCE("_ZN2nn2vi10InitializeEv", "display service init; the host has no vi service", void, h_vi_Init, void) {}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "open default display; the host returns a static object", Result, h_vi_OpenDisp, void **out) {
    *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "create layer; the host returns a static object", Result, h_vi_CreateLayer, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "native window handle, passed to the NVN window builder; the host returns a static object", Result, h_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}
// Link's Awakening also crops its layer and tears the display down on exit; the SDK's versions dereference the vi service objects the host never made (SetLayerCrop: SIGSEGV at +0x64).
// The crop is kept for the Vulkan present (hle_vk.m): Super Mario Galaxy keeps a 1920x1080 swapchain in handheld mode, draws into its top-left 1280x720 and crops the layer
// to that every frame. NVN games crop through their window instead (nvnWindowSetCrop), so their present does not read it. Logged only when it changes
static _Atomic uint64_t g_crop;   // x | y << 16 | w << 32 | h << 48, 0 = none
void dm_vi_layer_crop(int c[4]) { uint64_t v = g_crop; c[0] = (int)(v & 0xffff); c[1] = (int)(v >> 16 & 0xffff); c[2] = (int)(v >> 32 & 0xffff); c[3] = (int)(v >> 48); }
HLE_FORCE("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", "layer crop; the host records it and the Vulkan present crops by it", Result, h_vi_SetCrop, void *layer, int x, int y, int w, int h) {
    (void)layer;
    uint64_t v = x < 0 || y < 0 || w <= 0 || h <= 0 || x > 0xffff || y > 0xffff || w > 0xffff || h > 0xffff ? 0 : (uint64_t)x | (uint64_t)y << 16 | (uint64_t)w << 32 | (uint64_t)h << 48;
    if (atomic_exchange(&g_crop, v) != v) LOG("vi::SetLayerCrop %d,%d %dx%d", x, y, w, h);
    return RES_OK;
}
// Super Mario Odyssey sets its layer's scaling mode; the SDK's version uses the vi service objects the host never made
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", "layer scaling mode; the host present does its own scaling", Result, h_vi_SetScaling, void *layer, int mode) {
    (void)layer; LOG("vi::SetLayerScalingMode %d (ignored)", mode); return RES_OK;
}
// Unreal Engine paces its frames by the layer's frame number (Octopath Traveler 0, main+0x10140e8); the SDK's versions read the Android surface of the vi service the host never made
// (SIGSEGV in android::Surface::getQueuedFrameNumber). The host's frame number is the count of frames the game has presented
uint64_t dm_nvn_frame_count(void);
HLE_FORCE("_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", "the layer's latest frame number: the frames presented so far", Result, h_vi_FrameNo, uint64_t *out, const void *layer) {
    (void)layer; *out = dm_nvn_frame_count(); return RES_OK;
}
HLE_FORCE("_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", "the layer's frame history: none kept, 0 entries", int, h_vi_FrameHist, void *out, int n, const void *layer) {
    (void)out; (void)layer; static int once; if (!once++) LOG("vi::ListFrameHistory (%d entries asked): the host keeps no frame history, 0 returned", n);
    return 0;
}
// Rhythm Heaven Groove (SDK 21) asks for the built-in panel's identity at start (main+0xa9a4); the SDK's version calls through the vi service object the host never made (SIGSEGV at +0x20).
//   The game reads a u32 kind and, when it is 2 or more, 0x14 more bytes that it hands on; any other answer makes it hand on zeros. The host has no panel: an all-zero identifier, kind 0
HLE_FORCE("_ZN2nn2vi32GetDefaultDisplayPanelIdentifierEPNS0_15PanelIdentifierE", "built-in panel identity; the host has no panel: all zero", Result, h_vi_PanelId, void *out) {
    memset(out, 0, 0x18); return RES_OK;
}
HLE_FORCE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", "destroy layer; host static object", void, h_vi_DestroyLayer, void *layer) { (void)layer; }
HLE_FORCE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", "close display; host static object", void, h_vi_CloseDisp, void *disp) { (void)disp; }
HLE_FORCE("_ZN2nn2vi8FinalizeEv", "display service shutdown; the host has no vi service", void, h_vi_Fini, void) {}

// SDK 6-era entry points (Cyber Sleuth, NintendoSDK_OpenGL): the same calls with extra arguments, plus the display
//   queries the classic vi API exposes. The SDK's versions resolve the display through its ObjectManager's holder
//   lists — which only ever contain service-made holders — and fall back to nn::am::GetSelfController() == NULL on
//   the host (null vtable dereference inside CreateLayer). Same static objects as above.
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayEii", "create layer (explicit size); host static object", Result, h_vi_CreateLayerSz, void **out, void *disp, int w, int h) {
    (void)disp; LOG("vi::CreateLayer %dx%d", w, h);
    g_native.w = w > 0 ? w : 1280; g_native.h = h > 0 ? h : 720;
    *out = &g_layer; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayENS_4util10BitFlagSetILi32ENS0_10LayerFlagsEEE", "create layer (flags); host static object", Result, h_vi_CreateLayerFl, void **out, void *disp, uint64_t flags) {
    (void)disp; (void)flags; *out = &g_layer; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11OpenDisplayEPPNS0_7DisplayEPKc", "open named display; the default display for any name", Result, h_vi_OpenDispName, void **out, const char *name) {
    LOG("vi::OpenDisplay \"%s\"", name ? name : "?"); *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", "list displays; one default display", int, h_vi_ListDisplays, void *out, int n) {
    if (out && n > 0) { memset(out, 0, 0x40); strcpy((char *)out, "Default"); }
    return 1;
}
// DisplayModeInfo { i32 width, i32 height, float refresh, ... }: the handheld panel, 1280x720 @ 60
static void vi_modeinfo(void *out) { uint32_t *m = out; memset(out, 0, 0x20); m[0] = 1280; m[1] = 720; *(float *)(m + 2) = 60.f; }
HLE_FORCE("_ZN2nn2vi14GetDisplayModeEPNS0_15DisplayModeInfoEPKNS0_7DisplayE", "current display mode; 1280x720 @ 60", Result, h_vi_GetMode, void *out, const void *disp) {
    (void)disp; vi_modeinfo(out); return RES_OK;
}
HLE_FORCE("_ZN2nn2vi16ListDisplayModesEPNS0_15DisplayModeInfoEiPKNS0_7DisplayE", "list display modes; one 1280x720 mode", int, h_vi_ListModes, void *out, int n, const void *disp) {
    (void)disp; if (out && n > 0) { vi_modeinfo(out); return 1; }
    return 0;
}
HLE_FORCE("_ZN2nn2vi14SetDisplayModeEPNS0_7DisplayEPKNS0_15DisplayModeInfoE", "set display mode; the one mode exists", Result, h_vi_SetMode, void *disp, const void *mode) {
    (void)disp; (void)mode; return RES_OK;
}
static Result vi_res(int *w, int *h) { if (w) *w = 1280; if (h) *h = 720; return RES_OK; }
HLE_FORCE("_ZN2nn2vi20GetDisplayResolutionEPiS1_PKNS0_7DisplayE", "display resolution; 1280x720", Result, h_vi_Res, int *w, int *h, const void *disp) { (void)disp; return vi_res(w, h); }
HLE_FORCE("_ZN2nn2vi27GetDisplayLogicalResolutionEPiS1_PKNS0_7DisplayE", "logical display resolution; 1280x720", Result, h_vi_LogRes, int *w, int *h, const void *disp) { (void)disp; return vi_res(w, h); }
HLE_FORCE("_ZN2nn2vi19GetDisplayUnderscanEPiPKNS0_7DisplayE", "display underscan; none", Result, h_vi_Underscan, int *u, const void *disp) { (void)disp; *u = 0; return RES_OK; }
HLE_FORCE("_ZN2nn2vi19SetDisplayUnderscanEPNS0_7DisplayEi", "set display underscan; no panel to set it on", Result, h_vi_SetUnderscan, void *disp, int u) { (void)disp; (void)u; return RES_OK; }
HLE_FORCE("_ZN2nn2vi24SetLayerSettingsDefaultsEPNS_4util10BitFlagSetILi32ENS0_10LayerFlagsEEE", "default layer flags; left alone", Result, h_vi_LayerFlagsDef, void *flags) { *(uint64_t *)flags = 0; return RES_OK; }
// Layer / display attributes: stored so getters echo back the setter's value; the host present ignores them all.
static int g_lz, g_lw = 1280, g_lh = 720; static float g_lx, g_ly, g_alpha = 1.f;
HLE_FORCE("_ZN2nn2vi13SetLayerAlphaEPNS0_5LayerEf", "layer alpha; recorded", Result, h_vi_SetAlpha, void *layer, float a) { (void)layer; g_alpha = a; return RES_OK; }
HLE_FORCE("_ZN2nn2vi15SetDisplayAlphaEPNS0_7DisplayEf", "display alpha; recorded", Result, h_vi_SetDispAlpha, void *disp, float a) { (void)disp; g_alpha = a; return RES_OK; }
HLE_FORCE("_ZN2nn2vi9SetLayerZEPNS0_5LayerEi", "layer z; recorded", Result, h_vi_SetZ, void *layer, int z) { (void)layer; g_lz = z; return RES_OK; }
HLE_FORCE("_ZN2nn2vi9GetLayerZEPiPKNS0_5LayerE", "layer z; the recorded value", Result, h_vi_GetZ, int *z, const void *layer) { (void)layer; *z = g_lz; return RES_OK; }
HLE_FORCE("_ZN2nn2vi12SetLayerSizeEPNS0_5LayerEii", "layer size; recorded", Result, h_vi_SetSize, void *layer, int w, int h) { (void)layer; g_lw = w; g_lh = h; return RES_OK; }
HLE_FORCE("_ZN2nn2vi16SetLayerPositionEPNS0_5LayerEff", "layer position; recorded", Result, h_vi_SetPos, void *layer, float x, float y) { (void)layer; g_lx = x; g_ly = y; return RES_OK; }
HLE_FORCE("_ZN2nn2vi18SetLayerVisibilityEPNS0_5LayerEb", "layer visibility; ignored", Result, h_vi_SetVis, void *layer, int v) { (void)layer; (void)v; return RES_OK; }
HLE_FORCE("_ZN2nn2vi15AddToLayerStackEPNS0_5LayerENS0_10LayerStackE", "layer stack add; ignored", Result, h_vi_AddStack, void *layer, int stack) { (void)layer; (void)stack; return RES_OK; }
HLE_FORCE("_ZN2nn2vi20RemoveFromLayerStackEPNS0_5LayerENS0_10LayerStackE", "layer stack remove; ignored", Result, h_vi_DelStack, void *layer, int stack) { (void)layer; (void)stack; return RES_OK; }
HLE_FORCE("_ZN2nn2vi20SetDisplayLayerStackEPNS0_7DisplayENS0_10LayerStackE", "display layer stack; ignored", Result, h_vi_DispStack, void *disp, int stack) { (void)disp; (void)stack; return RES_OK; }
HLE_FORCE("_ZN2nn2vi17GetZOrderCountMinEPKNS0_7DisplayE", "min layer count; 1", int64_t, h_vi_ZMin, const void *disp) { (void)disp; return 1; }
HLE_FORCE("_ZN2nn2vi17GetZOrderCountMaxEPKNS0_7DisplayE", "max layer count; 1", int64_t, h_vi_ZMax, const void *disp) { (void)disp; return 1; }
HLE_FORCE("_ZN2nn2vi24GetLayerIdWithValidationEPKNS0_5LayerE", "validated layer id; 1", uint64_t, h_vi_LayerId, const void *layer) { (void)layer; return 1; }
HLE_FORCE("_ZN2nn2vi26GetDisplayIdWithValidationEPKNS0_7DisplayE", "validated display id; 1", uint64_t, h_vi_DispId, const void *disp) { (void)disp; return 1; }
HLE_FORCE("_ZN2nn2vi17SetDefaultDisplayEPNS0_7DisplayE", "set default display; already the only one", Result, h_vi_SetDefDisp, void *disp) { (void)disp; return RES_OK; }
HLE_FORCE("_ZN2nn2vi20SetContentVisibilityEb", "content visibility; ignored", Result, h_vi_ContentVis, int v) { (void)v; return RES_OK; }
HLE_FORCE("_ZN2nn2vi10DumpMemoryEv", "vi memory dump; nothing to dump", void, h_vi_DumpMem, void) {}
// Display events: a kernel event object, never signaled (no hotplug / mode change / vsync-driven logic on the host)
HLE_FORCE("_ZN2nn2vi26GetDisplayModeChangedEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "mode-changed event: bound, never signaled", Result, h_vi_ModeChgEv, void *ev, void *disp) { (void)disp; hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn2vi22GetDisplayHotplugEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "hotplug event: bound, never signaled", Result, h_vi_HotplugEv, void *ev, void *disp) { (void)disp; hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn2vi20GetDisplayErrorEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "display error event: bound, never signaled", Result, h_vi_DispErrEv, void *ev, void *disp) { (void)disp; hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn2vi28GetDisplayVsyncEventForDebugEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "debug vsync event: bound, never signaled", Result, h_vi_VsyncDbgEv, void *ev, void *disp) { (void)disp; hle_bind_system_event(ev, 0); return RES_OK; }
HLE_FORCE("_ZN2nn2vi22GetDisplayHotplugStateEPNS0_12HotplugStateEPKNS0_7DisplayE", "hotplug state: connected", Result, h_vi_Hotplug, void *out, const void *disp) { (void)disp; *(uint32_t *)out = 1; return RES_OK; }
HLE_FORCE("_ZN2nn2vi22GetCompositorErrorInfoEPNS0_15CompositorErrorEiPNS0_7DisplayE", "compositor errors; none", int, h_vi_CompErr, void *out, int n, void *disp) { (void)out; (void)n; (void)disp; return 0; }
