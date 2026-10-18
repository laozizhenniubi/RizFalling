// dllmain.cpp — d3d11.dll 代理：转发系统 d3d11 导出并触发叠加层 hook
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cstdio>
#include <cstring>

typedef DWORD D3DCOLOR;  // d3d11.h 不带 d3d9 的类型

// 定义在 overlay.cpp
extern void ensure_hook();

static HMODULE g_real = nullptr;

extern "C" {
// 供汇编 stub 跳转的真实函数指针
void* gp_D3D11CoreCreateDevice = nullptr;
void* gp_D3D11CoreCreateLayeredDevice = nullptr;
void* gp_D3D11CoreGetLayeredDeviceSize = nullptr;
void* gp_D3D11CoreRegisterLayers = nullptr;
void* gp_D3D11DeviceSetDebugName1 = nullptr;
}

static void* resolve_one(const char* name) {
    if (!g_real)
        g_real = LoadLibraryW(L"C:\\Windows\\System32\\d3d11.dll");
    return g_real ? (void*)GetProcAddress(g_real, name) : nullptr;
}

static void resolve_all() {
    static bool done = false;
    if (done) return;
    done = true;
    gp_D3D11CoreCreateDevice = resolve_one("D3D11CoreCreateDevice");
    gp_D3D11CoreCreateLayeredDevice = resolve_one("D3D11CoreCreateLayeredDevice");
    gp_D3D11CoreGetLayeredDeviceSize = resolve_one("D3D11CoreGetLayeredDeviceSize");
    gp_D3D11CoreRegisterLayers = resolve_one("D3D11CoreRegisterLayers");
    gp_D3D11DeviceSetDebugName1 = resolve_one("D3D11DeviceSetDebugName1");
}

// ---- 内部导出：直接跳转到真实函数（不关心中间签名）----
extern "C" __declspec(dllexport) __attribute__((naked)) void D3D11CoreCreateDevice() {
    __asm__ __volatile__("jmp *gp_D3D11CoreCreateDevice(%rip)");
}
extern "C" __declspec(dllexport) __attribute__((naked)) void D3D11CoreCreateLayeredDevice() {
    __asm__ __volatile__("jmp *gp_D3D11CoreCreateLayeredDevice(%rip)");
}
extern "C" __declspec(dllexport) __attribute__((naked)) void D3D11CoreGetLayeredDeviceSize() {
    __asm__ __volatile__("jmp *gp_D3D11CoreGetLayeredDeviceSize(%rip)");
}
extern "C" __declspec(dllexport) __attribute__((naked)) void D3D11CoreRegisterLayers() {
    __asm__ __volatile__("jmp *gp_D3D11CoreRegisterLayers(%rip)");
}
extern "C" __declspec(dllexport) __attribute__((naked)) void D3D11DeviceSetDebugName1() {
    __asm__ __volatile__("jmp *gp_D3D11DeviceSetDebugName1(%rip)");
}

// ---- 设备创建（游戏主路径）----
extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDevice(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    resolve_all();
    ensure_hook();
    using Fn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                const D3D_FEATURE_LEVEL*, UINT, UINT,
                                ID3D11Device**, D3D_FEATURE_LEVEL*,
                                ID3D11DeviceContext**);
    static Fn fn = (Fn)resolve_one("D3D11CreateDevice");
    if (!fn) return E_FAIL;
    return fn(pAdapter, DriverType, Software, Flags, pFeatureLevels,
              FeatureLevels, SDKVersion, ppDevice, pFeatureLevel,
              ppImmediateContext);
}

extern "C" __declspec(dllexport) HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
    resolve_all();
    ensure_hook();
    using Fn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                const D3D_FEATURE_LEVEL*, UINT, UINT,
                                const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                ID3D11Device**, D3D_FEATURE_LEVEL*,
                                ID3D11DeviceContext**);
    static Fn fn = (Fn)resolve_one("D3D11CreateDeviceAndSwapChain");
    if (!fn) return E_FAIL;
    return fn(pAdapter, DriverType, Software, Flags, pFeatureLevels,
              FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain,
              ppDevice, pFeatureLevel, ppImmediateContext);
}

// ---- D3DPERF / 调试导出（Unity 会调用）----
extern "C" __declspec(dllexport) int WINAPI D3DPERF_BeginEvent(D3DCOLOR col, LPCWSTR name) {
    using Fn = int(WINAPI*)(D3DCOLOR, LPCWSTR);
    static Fn fn = (Fn)resolve_one("D3DPERF_BeginEvent");
    return fn ? fn(col, name) : 0;
}
extern "C" __declspec(dllexport) int WINAPI D3DPERF_EndEvent() {
    using Fn = int(WINAPI*)();
    static Fn fn = (Fn)resolve_one("D3DPERF_EndEvent");
    return fn ? fn() : 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus() {
    using Fn = DWORD(WINAPI*)();
    static Fn fn = (Fn)resolve_one("D3DPERF_GetStatus");
    return fn ? fn() : 0;
}
extern "C" __declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame() {
    using Fn = BOOL(WINAPI*)();
    static Fn fn = (Fn)resolve_one("D3DPERF_QueryRepeatFrame");
    return fn ? fn() : FALSE;
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR col, LPCWSTR name) {
    using Fn = void(WINAPI*)(D3DCOLOR, LPCWSTR);
    static Fn fn = (Fn)resolve_one("D3DPERF_SetMarker");
    if (fn) fn(col, name);
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD opts) {
    using Fn = void(WINAPI*)(DWORD);
    static Fn fn = (Fn)resolve_one("D3DPERF_SetOptions");
    if (fn) fn(opts);
}
extern "C" __declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR col, LPCWSTR name) {
    using Fn = void(WINAPI*)(D3DCOLOR, LPCWSTR);
    static Fn fn = (Fn)resolve_one("D3DPERF_SetRegion");
    if (fn) fn(col, name);
}
extern "C" __declspec(dllexport) void WINAPI DebugSetMute() {
    using Fn = void(WINAPI*)();
    static Fn fn = (Fn)resolve_one("DebugSetMute");
    if (fn) fn();
}
extern "C" __declspec(dllexport) void WINAPI DisableD3D11DebugLayer() {
    using Fn = void(WINAPI*)();
    static Fn fn = (Fn)resolve_one("DisableD3D11DebugLayer");
    if (fn) fn();
}
extern "C" __declspec(dllexport) void WINAPI EnableExperimentalShaderFeatures() {
    using Fn = void(WINAPI*)();
    static Fn fn = (Fn)resolve_one("EnableExperimentalShaderFeatures");
    if (fn) fn();
}

static DWORD WINAPI boot_thread(LPVOID) {
    Sleep(1500);   // 等 LoadLibrary 的 loader lock 释放
    resolve_all();
    ensure_hook();
    return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        CreateThread(nullptr, 0, boot_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
