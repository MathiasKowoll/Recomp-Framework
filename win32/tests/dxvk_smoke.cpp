// Smoke test for the native DXVK d3d9 build on macOS with the host WSI: an AppKit window with a CAMetalLayer,
// a D3D9 device on Vulkan (KosmicKrisp), clear + present 60 frames. Passes when every call succeeds.
//   build: clang++ -std=c++17 with DXVK's include/, include/native/windows and include/native/directx, plus
//   dxvk_smoke_window.mm (the AppKit half), linked against libdxvk_d3d9.dylib (win32/dxvk.mk)
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#include <d3d9.h>
#include "native/wsi/native_host.h"

extern "C" void *smoke_window(int w, int h);
extern "C" void *smoke_layer(void);
extern "C" void smoke_pump(void);
static void cb_size(void *, uint32_t *w, uint32_t *h) { *w = 640; *h = 360; }
static void *cb_layer(void *) { return smoke_layer(); }
static int cb_is_window(void *h) { return h != nullptr; }

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL %s -> 0x%08x\n", #x, (unsigned)hr_); return 1; } } while (0)

int main() {
    setenv("DXVK_WSI_DRIVER", "Host", 0);
    void *w = smoke_window(640, 360);
    DxvkHostWsiCallbacks cb = {};
    cb.version = 1; cb.get_window_size = cb_size; cb.get_metal_layer = cb_layer; cb.is_window = cb_is_window;
    dxvk_host_wsi_set_callbacks(&cb);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 failed\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 id = {};
    CHECK(d3d->GetAdapterIdentifier(0, 0, &id));
    printf("adapter: %s (%s)\n", id.Description, id.Driver);
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = 640; pp.BackBufferHeight = 360; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = (HWND)w; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9 *dev = nullptr;
    CHECK(d3d->CreateDevice(0, D3DDEVTYPE_HAL, (HWND)w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));
    for (int f = 0; f < 60; f++) {
        smoke_pump();
        CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(f * 4, 40, 120), 1.0f, 0));
        CHECK(dev->BeginScene());
        CHECK(dev->EndScene());
        CHECK(dev->Present(nullptr, nullptr, nullptr, nullptr));
    }
    dev->Release(); d3d->Release();
    printf("OK: 60 frames presented through DXVK d3d9\n");
    return 0;
}
