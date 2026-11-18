// The small DLLs: winmm, advapi32, shell32, ole32/oleaut32, gdi32, imm32, iphlpapi, ws2_32, XInput, DirectInput,
// Steam (offline), and the not-yet-implemented media and graphics entry points (they fail cleanly and log).
#include "rt32.h"
#include <time.h>
#include <sys/stat.h>
#include <mach/mach_time.h>

// ---- winmm ----
SHIM(winmm_timeGetTime, 0) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (uint32_t)(mach_absolute_time() * tb.numer / tb.denom / 1000000ull);
}
SHIM(winmm_timeBeginPeriod, 1) { return 0; }
SHIM(winmm_timeEndPeriod, 1) { return 0; }

// ---- advapi32: no registry ----
SHIM(advapi32_RegOpenKeyExA, 5) { return 2; }   // ERROR_FILE_NOT_FOUND
SHIM(advapi32_RegQueryValueExA, 6) { return 2; }
SHIM(advapi32_RegCloseKey, 1) { return 0; }

// ---- shell32 ----
SHIM(shell32_SHGetFolderPathA, 5) {
    // CSIDL_PERSONAL (5), CSIDL_APPDATA (0x1A), CSIDL_LOCAL_APPDATA (0x1C)...: all under the app's data folder,
    // which rt_host_path maps back (rt_game.docs_dir -> rt_data_dir)
    const char *p = rt_game.docs_dir;
    snprintf(G2H(ARG(4)), 260, "%s", p);
    mkdir(rt_data_dir, 0755);
    return 0;
}
SHIM(shell32_DragAcceptFiles, 2) { return 0; }

// ---- ole32 / oleaut32 ----
SHIM(ole32_CoInitialize, 1) { return 0; }
SHIM(ole32_CoUninitialize, 0) { return 0; }
SHIM(ole32_CoCreateInstance, 5) {   // no COM servers: REGDB_E_CLASSNOTREG
    const uint8_t *g = gp(ARG(0));
    if (g) rt_log("[ole32] CoCreateInstance {%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x} from 0x%x -> not registered",
                  *(uint32_t *)g, *(uint16_t *)(g + 4), *(uint16_t *)(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15], rd32(c->esp));
    if (ARG(4)) wr32(ARG(4), 0);
    return 0x80040154u;
}
SHIM(ole32_CoSetProxyBlanket, 8) { return 0; }
SHIM(ole32_CoTaskMemAlloc, 1) { return gmalloc(ARG(0)); }
SHIM(ole32_CoTaskMemFree, 1) { gfree(ARG(0)); return 0; }
SHIM(oleaut32_ord2, 1) {   // SysAllocString: length-prefixed UTF-16
    size_t n = w_len(WSTR(0));
    uint32_t p = gmalloc((uint32_t)(n * 2 + 6));
    wr32(p, (uint32_t)(n * 2)); memcpy(G2H(p + 4), WSTR(0), n * 2); wr16(p + 4 + (uint32_t)n * 2, 0);
    return p + 4;
}
SHIM(oleaut32_ord6, 1) { if (ARG(0)) gfree(ARG(0) - 4); return 0; }   // SysFreeString

// ---- gdi32 / imm32 ----
SHIM(gdi32_GetStockObject, 1) { return 0x50000u + ARG(0); }
SHIM(imm32_ImmGetContext, 1) { return 0; }
SHIM(imm32_ImmReleaseContext, 2) { return 1; }
SHIM(imm32_ImmGetCompositionStringA, 4) { return 0; }
SHIM(imm32_ImmGetDefaultIMEWnd, 1) { return 0; }

// ---- networking: the game only checks for an adapter ----
SHIM(ws2_32_ord115, 2) { if (ARG(1)) { memset(G2H(ARG(1)), 0, 400); wr16(ARG(1), 0x0202); wr16(ARG(1) + 2, 0x0202); } return 0; }   // WSAStartup
SHIM(ws2_32_ord116, 0) { return 0; }                                                                                              // WSACleanup
SHIM(iphlpapi_GetAdaptersAddresses, 5) { if (ARG(4)) wr32(ARG(4), 0); return 232; }   // ERROR_NO_DATA
SHIM(iphlpapi_GetIfEntry, 1) { return 50; }   // ERROR_NOT_SUPPORTED

// ---- XInput 1.4 (ordinals): no controller yet (GameController bridge comes with the input layer) ----
SHIM(xinput1_4_ord2, 2) { return 1167; }   // XInputGetState -> ERROR_DEVICE_NOT_CONNECTED
SHIM(xinput1_4_ord3, 2) { return 1167; }   // XInputSetState
SHIM(xinput1_4_ord4, 3) { return 1167; }   // XInputGetCapabilities
SHIM(xinput1_4_ord5, 1) { return 0; }      // XInputEnable

// ---- not yet implemented: fail the way a missing component does on Windows, and say so once ----
#define NOTYET(sym, n, ret, what) SHIM(sym, n) { static int once; if (!once++) rt_log("[todo] %s: %s", #sym, what); return ret; }
NOTYET(wmvcore_WMCreateWriter, 2, 0x80004005u, "WMCreateWriter")
NOTYET(wmvcore_WMCreateProfileManager, 1, 0x80004005u, "WMCreateProfileManager")
NOTYET(avifil32_AVIFileInit, 0, 0, "AVIFileInit")
NOTYET(avifil32_AVIFileExit, 0, 0, "AVIFileExit")
NOTYET(gdiplus_GdiplusStartup, 3, 1, "GDI+ (screenshot encoding) -> GenericError")
NOTYET(gdiplus_GdiplusShutdown, 1, 0, "GdiplusShutdown")
// d3d9: d3d9_bridge.cpp (host d3d9 backends such as DXVK)

// ---- Steam: steam32.c
