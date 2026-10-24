// overlay.cpp — 进程内 D3D11 叠加层：ImGui 渲染 + 自包含谱面/配置 + Present hook
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <objbase.h>
#include <shlobj.h>

#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "chart_loader.h"
#include "audio_align.h"

static void rflog(const char* msg);

// ---------------- 全局 ----------------
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11Texture2D* g_rtv_tex = nullptr;
static bool g_imgui_inited = false;
static bool g_menu_open = false;
static bool g_show_charts = false;
static std::vector<std::string> g_osz_files;
static double g_osz_scan_t = 0.0;
static HWND g_game_hwnd = nullptr;
static float g_dpi_scale = 1.0f;
static bool g_menu_visible_for_input = false;

// 数据目录：%LOCALAPPDATA%\RizFalling
static std::wstring& data_dir() {
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t base[MAX_PATH] = {0};
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        dir = (n ? base : L".");
        dir += L"\\RizFalling";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

static std::wstring cfg_path() { return data_dir() + L"\\native.ini"; }
static std::wstring dir_file() { return data_dir() + L"\\chart_dir.txt"; }

static void rflog(const char* msg) {
    static CRITICAL_SECTION cs;
    static bool init = false;
    if (!init) {
        InitializeCriticalSection(&cs);
        init = true;
    }
    EnterCriticalSection(&cs);
    std::wstring p = data_dir() + L"\\native.log";
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD n = 0;
        WriteFile(h, msg, (DWORD)strlen(msg), &n, nullptr);
        char nl[2] = {13, 10};
        WriteFile(h, nl, 2, &n, nullptr);
        CloseHandle(h);
    }
    LeaveCriticalSection(&cs);
}

// ---------------- 配置 / 谱面状态 ----------------
struct RfCfg {
    float speed_frac = 0.76f;
    float lane_w_frac = 96.0f / 1152.0f;
    float gap_frac = 10.0f / 1152.0f;
    float judge_frac = 0.16f;
    float note_frac = 0.72f;
    float lane_a = 25.0f / 255.0f;
    float edge_a = 0.55f;
    float note_a = 1.0f;
    float hold_a = 0.45f;
    int note_color = 0x7FD8FF;
    int hold_color = 0x4FA8E0;
    int judge_color = 0xFFFFFF;
    int lane_color = 0xFFFFFF;
    float lane_off_frac = 0.0f;
    float judge_off_frac = 0.0f;
    int dark_on = 1;
    float dark_a = 0.40f;
    int scale_pct = 100;
    int lane_vis = 1;
    int visible = 1;
    int offset_ms = 0;
    int mirror = 0;
    int hold_tail = 0;    // 0 尖尾 1 空缺 2 无
    int auto_align = 0;
    int tap_skin = 0;     // 0 圆球 1 圆环 2 方块 3 菱形
    int hold_skin = 0;    // 0 实心条 1 描边条 2 管道
};

static RfCfg g_cfg;
static bool g_cfg_loaded = false;
static bool g_cfg_dirty = false;
static double g_cfg_save_t = 0.0;

static std::wstring g_osz_dir;   // 首次由用户选择谱面文件夹
static std::wstring g_dir_pending;
static volatile LONG g_dir_ready = 0;
static CRITICAL_SECTION g_dir_cs;
static bool g_dir_cs_init = false;

static ChartData g_chart;
static std::wstring g_osz_path;
static std::string g_osz_name;
static int g_diff_idx = -1;
static int g_diff_count = 0;
static bool g_loaded = false;
static bool g_anchored = false;
static bool g_j_used = false;
static double g_song_time = 0.0;
static double g_stamp = 0.0;
static bool g_j_prev = false;

static int g_anchor_vk = 0x4A;
static int g_anchor_any = 0;
static int g_hold_enabled = 0;
static int g_hold_vk = 0;
static bool g_holding = false;
static int g_capture_target = 0;
static bool g_capture_key = false;
static bool g_key_prev[256] = {0};
static bool g_any_prev[256] = {0};
static int g_key_list[64];
static int g_key_list_n = 0;

static bool g_calib = false;
static std::vector<double> g_calib_diffs;
static char g_msg[200] = {0};
static double g_msg_t = 0.0;

static void set_msg(const char* msg) {
    strncpy(g_msg, msg, sizeof(g_msg) - 1);
    g_msg[sizeof(g_msg) - 1] = 0;
    g_msg_t = GetTickCount64() / 1000.0;
    rflog(msg);
}

static void release_held_key() {
    if (g_holding && g_hold_vk) {
        keybd_event((BYTE)g_hold_vk, 0, KEYEVENTF_KEYUP, 0);
        g_holding = false;
    }
}

static bool is_anchor_key(int vk) {
    if (vk >= 'A' && vk <= 'Z') return true;
    if (vk >= '0' && vk <= '9') return true;
    if (vk >= 0xBA && vk <= 0xC0) return true;
    if (vk >= 0xDB && vk <= 0xDE) return true;
    if (vk == 0xE2) return true;
    return false;
}

static void key_name_utf8(int vk, char* out, int n) {
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    wchar_t w[64] = {0};
    if (!GetKeyNameTextW((LONG)(sc << 16), w, 64) || !w[0])
        wsprintfW(w, L"VK%d", vk);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, nullptr, nullptr);
}

static void read_dir_file() {
    HANDLE h = CreateFileW(dir_file().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[2048] = {0};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);
    if (got == 0) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, nullptr, 0);
    if (n <= 0) return;
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, &w[0], n);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n'))
        w.pop_back();
    if (!w.empty()) g_osz_dir = w;
}

static void write_dir_file() {
    char buf[4096] = {0};
    int n = WideCharToMultiByte(CP_UTF8, 0, g_osz_dir.c_str(), -1, buf,
                                sizeof(buf) - 1, nullptr, nullptr);
    if (n <= 1) return;
    HANDLE h = CreateFileW(dir_file().c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, buf, (DWORD)(n - 1), &wr, nullptr);
    CloseHandle(h);
}

static DWORD WINAPI pick_folder_proc(LPVOID param) {
    HWND owner = (HWND)param;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择包含 .osz 的文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH] = {0};
        if (SHGetPathFromIDListW(pidl, path) && path[0] && g_dir_cs_init) {
            EnterCriticalSection(&g_dir_cs);
            g_dir_pending = path;
            InterlockedExchange(&g_dir_ready, 1);
            LeaveCriticalSection(&g_dir_cs);
        }
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

static void cfg_load() {
    if (g_cfg_loaded) return;
    g_cfg_loaded = true;
    read_dir_file();
    auto ri = [](const wchar_t* key, int def) {
        return GetPrivateProfileIntW(L"view", key, def, cfg_path().c_str());
    };
    g_cfg.speed_frac = ri(L"speed_milli", 760) / 1000.0f;
    g_cfg.lane_w_frac = ri(L"lane_w_milli", 83) / 1000.0f;
    g_cfg.gap_frac = ri(L"gap_milli", 9) / 1000.0f;
    g_cfg.judge_frac = ri(L"judge_milli", 160) / 1000.0f;
    g_cfg.note_frac = ri(L"note_milli", 720) / 1000.0f;
    g_cfg.lane_a = ri(L"lane_a_milli", 98) / 1000.0f;
    g_cfg.edge_a = ri(L"edge_milli", 550) / 1000.0f;
    g_cfg.note_a = ri(L"note_a_milli", 1000) / 1000.0f;
    g_cfg.hold_a = ri(L"hold_milli", 450) / 1000.0f;
    g_cfg.note_color = ri(L"note_color", 0x7FD8FF);
    g_cfg.hold_color = ri(L"hold_color", 0x4FA8E0);
    g_cfg.judge_color = ri(L"judge_color", 0xFFFFFF);
    g_cfg.lane_color = ri(L"lane_color", 0xFFFFFF);
    g_cfg.lane_off_frac = ri(L"lane_off_milli", 0) / 1000.0f;
    g_cfg.judge_off_frac = ri(L"judge_off_milli", 0) / 1000.0f;
    g_cfg.dark_on = ri(L"dark_on", 1);
    g_cfg.dark_a = ri(L"dark_milli", 400) / 1000.0f;
    g_cfg.scale_pct = ri(L"scale", 100);
    g_cfg.lane_vis = ri(L"lane_vis", 1);
    g_cfg.visible = ri(L"visible", 1);
    g_cfg.offset_ms = ri(L"offset_ms", 0);
    g_cfg.mirror = ri(L"mirror", 0) ? 1 : 0;
    g_cfg.hold_tail = ri(L"hold_tail", 0);
    if (g_cfg.hold_tail < 0 || g_cfg.hold_tail > 2) g_cfg.hold_tail = 0;
    g_cfg.tap_skin = ri(L"tap_skin", 0);
    if (g_cfg.tap_skin < 0 || g_cfg.tap_skin > 3) g_cfg.tap_skin = 0;
    g_cfg.hold_skin = ri(L"hold_skin", 0);
    if (g_cfg.hold_skin < 0 || g_cfg.hold_skin > 2) g_cfg.hold_skin = 0;
    g_anchor_vk = ri(L"anchor_vk", 0x4A);
    if (!is_anchor_key(g_anchor_vk)) g_anchor_vk = 0x4A;
    g_anchor_any = ri(L"anchor_any", 0) ? 1 : 0;
    g_hold_enabled = ri(L"hold_enabled", 0) ? 1 : 0;
    g_hold_vk = ri(L"hold_vk", 0);
    if (g_hold_vk && !is_anchor_key(g_hold_vk)) g_hold_vk = 0;
    g_key_list_n = 0;
    for (int vk = 8; vk < 256 && g_key_list_n < 64; vk++)
        if (is_anchor_key(vk)) g_key_list[g_key_list_n++] = vk;
}

static void cfg_save() {
    auto wi = [](const wchar_t* key, int v) {
        wchar_t b[32];
        wsprintfW(b, L"%d", v);
        WritePrivateProfileStringW(L"view", key, b, cfg_path().c_str());
    };
    wi(L"speed_milli", (int)(g_cfg.speed_frac * 1000));
    wi(L"lane_w_milli", (int)(g_cfg.lane_w_frac * 1000));
    wi(L"gap_milli", (int)(g_cfg.gap_frac * 1000));
    wi(L"judge_milli", (int)(g_cfg.judge_frac * 1000));
    wi(L"note_milli", (int)(g_cfg.note_frac * 1000));
    wi(L"lane_a_milli", (int)(g_cfg.lane_a * 1000));
    wi(L"edge_milli", (int)(g_cfg.edge_a * 1000));
    wi(L"note_a_milli", (int)(g_cfg.note_a * 1000));
    wi(L"hold_milli", (int)(g_cfg.hold_a * 1000));
    wi(L"note_color", g_cfg.note_color);
    wi(L"hold_color", g_cfg.hold_color);
    wi(L"judge_color", g_cfg.judge_color);
    wi(L"lane_color", g_cfg.lane_color);
    wi(L"lane_off_milli", (int)(g_cfg.lane_off_frac * 1000));
    wi(L"judge_off_milli", (int)(g_cfg.judge_off_frac * 1000));
    wi(L"dark_on", g_cfg.dark_on);
    wi(L"dark_milli", (int)(g_cfg.dark_a * 1000));
    wi(L"scale", g_cfg.scale_pct);
    wi(L"lane_vis", g_cfg.lane_vis);
    wi(L"visible", g_cfg.visible);
    wi(L"offset_ms", g_cfg.offset_ms);
    wi(L"anchor_vk", g_anchor_vk);
    wi(L"mirror", g_cfg.mirror);
    wi(L"hold_tail", g_cfg.hold_tail);
    wi(L"tap_skin", g_cfg.tap_skin);
    wi(L"hold_skin", g_cfg.hold_skin);
    wi(L"auto_align", g_cfg.auto_align);
    wi(L"anchor_any", g_anchor_any);
    wi(L"hold_enabled", g_hold_enabled);
    wi(L"hold_vk", g_hold_vk);
}

static bool utf8_to_wide(const std::string& s, std::wstring& w) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return false;
    w.resize(n - 1);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return true;
}

static void load_chart_at(const std::string& osz_utf8, int diff) {
    if (g_osz_dir.empty()) {
        set_msg("请先在「谱面文件夹…」选择 osz 目录");
        return;
    }
    std::wstring wname;
    if (!utf8_to_wide(osz_utf8, wname)) {
        set_msg("路径编码失败");
        return;
    }
    std::wstring w = g_osz_dir;
    if (!w.empty() && w.back() != L'/' && w.back() != L'\\') w += L'/';
    w += wname;
    std::vector<DiffEntry> diffs;
    if (!chart_list_diffs(w, diffs)) {
        set_msg("osz 打开失败");
        return;
    }
    if (diff < 0 || diff >= (int)diffs.size()) {
        set_msg("难度序号越界");
        return;
    }
    ChartData c;
    if (!chart_load(w, diff, c)) {
        set_msg("谱面解析失败");
        return;
    }
    g_chart = std::move(c);
    g_osz_path = w;
    g_osz_name = osz_utf8;
    g_diff_idx = diff;
    g_diff_count = (int)diffs.size();
    g_loaded = true;
    g_anchored = false;
    g_j_used = false;
    g_song_time = 0.0;
    g_stamp = GetTickCount64() / 1000.0;
    char b[220];
    wsprintfA(b, "chart loaded: %s [%s] notes=%d first_ms=%d",
              osz_utf8.c_str(), diffs[diff].name.c_str(),
              (int)g_chart.notes.size(), (int)(g_chart.first_note * 1000));
    set_msg(b);
    g_calib = false;
    std::vector<unsigned char> ogg;
    if (chart_read_ogg(w, ogg)) {
        aa_set_reference_ogg(std::move(ogg));
        aa_enable(g_cfg.auto_align != 0);
    }
}

static void do_anchor_now() {
    if (!g_loaded || g_chart.notes.empty()) {
        set_msg("先加载谱面");
        return;
    }
    g_song_time = g_chart.first_note;
    g_stamp = GetTickCount64() / 1000.0;
    g_anchored = true;
    g_j_used = true;
    if (g_cfg.auto_align) aa_manual_anchor(g_song_time, g_stamp);
    set_msg("已锚定：现在 = 首音");
}

static void scan_osz_files() {
    g_osz_files.clear();
    if (g_osz_dir.empty()) {
        g_osz_scan_t = GetTickCount64() / 1000.0;
        return;
    }
    WIN32_FIND_DATAW fd;
    std::wstring pattern = g_osz_dir;
    if (!pattern.empty() && pattern.back() != L'/' && pattern.back() != L'\\')
        pattern += L'/';
    pattern += L"*.osz";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        g_osz_scan_t = GetTickCount64() / 1000.0;
        return;
    }
    do {
        char utf8[512] = {0};
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, utf8, 511,
                            nullptr, nullptr);
        g_osz_files.push_back(utf8);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(g_osz_files.begin(), g_osz_files.end());
    g_osz_scan_t = GetTickCount64() / 1000.0;
}

static ImU32 rgbs(int c, float a) {
    int aa = (int)(a * 255.0f);
    if (aa < 0) aa = 0;
    if (aa > 255) aa = 255;
    return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, aa);
}

// ---------------- 绘制 ----------------
// 球皮：tap / hold 头共用（a 为 0..1 渐隐系数）
static void draw_tap(ImDrawList* dl, float cx, float y, float r, float a,
                     const RfCfg& c) {
    ImU32 col = rgbs(c.note_color, c.note_a * a);
    ImU32 hi = IM_COL32(255, 255, 255, (int)(120 * a));
    switch (c.tap_skin) {
    case 1: {   // 圆环
        float th = r * 0.30f;
        if (th < 2.5f) th = 2.5f;
        dl->AddCircle(ImVec2(cx, y), r * 0.78f, col, 0, th);
        dl->AddCircleFilled(ImVec2(cx, y), r * 0.22f, col);
        break;
    }
    case 2: {   // 方块
        float e = r * 0.82f;
        dl->AddRectFilled(ImVec2(cx - e, y - e), ImVec2(cx + e, y + e), col,
                          r * 0.18f);
        dl->AddRectFilled(ImVec2(cx - e * 0.55f, y - e * 0.55f),
                          ImVec2(cx - e * 0.1f, y - e * 0.1f), hi);
        break;
    }
    case 3: {   // 菱形
        dl->AddQuadFilled(ImVec2(cx, y - r * 0.95f),
                          ImVec2(cx + r * 0.85f, y),
                          ImVec2(cx, y + r * 0.95f),
                          ImVec2(cx - r * 0.85f, y), col);
        dl->AddCircleFilled(ImVec2(cx, y), r * 0.20f, hi);
        break;
    }
    default: {  // 圆球
        dl->AddCircleFilled(ImVec2(cx, y), r, col);
        dl->AddCircleFilled(ImVec2(cx - r * 0.28f, y - r * 0.28f), r * 0.32f,
                            hi);
        break;
    }
    }
}

// 砖皮：hold 主体
static void draw_hold_body(ImDrawList* dl, float cx, float top, float bot,
                           float hold_w, const RfCfg& c) {
    if (bot <= top) return;
    ImU32 hcol = rgbs(c.hold_color, c.hold_a);
    switch (c.hold_skin) {
    case 1: {   // 描边条
        ImU32 body = rgbs(c.hold_color, c.hold_a * 0.45f);
        dl->AddRectFilled(ImVec2(cx - hold_w / 2, top),
                          ImVec2(cx + hold_w / 2, bot), body);
        ImU32 line = rgbs(c.hold_color,
                          (c.hold_a + 0.35f > 1.0f) ? 1.0f : c.hold_a + 0.35f);
        dl->AddRect(ImVec2(cx - hold_w / 2, top),
                    ImVec2(cx + hold_w / 2, bot), line, 0, 0, 2.0f);
        break;
    }
    case 2: {   // 管道
        float rail = hold_w * 0.22f;
        ImU32 mid = rgbs(c.hold_color, c.hold_a * 0.5f);
        dl->AddRectFilled(ImVec2(cx - hold_w / 2 + rail, top),
                          ImVec2(cx + hold_w / 2 - rail, bot), mid);
        dl->AddRectFilled(ImVec2(cx - hold_w / 2, top),
                          ImVec2(cx - hold_w / 2 + rail, bot), hcol);
        dl->AddRectFilled(ImVec2(cx + hold_w / 2 - rail, top),
                          ImVec2(cx + hold_w / 2, bot), hcol);
        break;
    }
    default: {  // 实心条
        dl->AddRectFilled(ImVec2(cx - hold_w / 2, top),
                          ImVec2(cx + hold_w / 2, bot), hcol);
        break;
    }
    }
}
static void draw_overlay(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                         ID3D11Texture2D* bb, HWND outHwnd) {
    static int dbg_frame = 0;
    if (dbg_frame < 3) { rflog("draw: frame"); dbg_frame++; }
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    const float W = (float)td.Width, H = (float)td.Height;
    cfg_load();
    const double now = GetTickCount64() / 1000.0;

    // ImGui 初始化
    if (!g_imgui_inited) {
        ImGui::CreateContext();
        ImGuiIO& iio = ImGui::GetIO();
        iio.IniFilename = nullptr;
        ImFontConfig fcfg;
        fcfg.OversampleH = 2;
        fcfg.OversampleV = 1;
        if (!iio.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 18.0f,
                                           &fcfg))
            rflog("imgui: chinese font missing, fallback");
        ImGui_ImplDX11_Init(dev, ctx);
        if (outHwnd && IsWindow(outHwnd)) {
            g_game_hwnd = outHwnd;
            RECT cr;
            GetClientRect(outHwnd, &cr);
            if (cr.right > 0)
                g_dpi_scale = (float)td.Width / (float)cr.right;
        }
        if (!g_dir_cs_init) {
            InitializeCriticalSection(&g_dir_cs);
            g_dir_cs_init = true;
        }
        atexit(release_held_key);
        atexit(aa_shutdown);
        g_imgui_inited = true;
        rflog("imgui: inited");
    }

    // 谱面文件夹切换
    if (g_dir_ready) {
        EnterCriticalSection(&g_dir_cs);
        std::wstring nd = g_dir_pending;
        InterlockedExchange(&g_dir_ready, 0);
        LeaveCriticalSection(&g_dir_cs);
        while (!nd.empty() && (nd.back() == L'/' || nd.back() == L'\\'))
            nd.pop_back();
        if (!nd.empty()) {
            g_osz_dir = nd;
            write_dir_file();
            scan_osz_files();
            g_show_charts = true;
            set_msg("谱面文件夹已切换");
        }
    }

    // 改键捕获
    if (g_capture_key) {
        for (int vk = 8; vk < 256; vk++) {
            bool k = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (k && !g_key_prev[vk] && is_anchor_key(vk)) {
                char kn[64], msg[128];
                key_name_utf8(vk, kn, sizeof(kn));
                if (g_capture_target == 1) {
                    g_hold_vk = vk;
                    wsprintfA(msg, "按住键已设为 %s", kn);
                } else {
                    g_anchor_vk = vk;
                    wsprintfA(msg, "锚定键已设为 %s", kn);
                }
                g_capture_key = false;
                g_capture_target = 0;
                g_cfg_dirty = true;
                set_msg(msg);
                g_key_prev[vk] = true;
                g_any_prev[vk] = true;
                g_j_prev = true;
                break;
            }
            g_key_prev[vk] = k;
        }
    }

    // 锚定：游戏前台 + 每次载入谱面只生效一次
    bool ready = g_loaded && !g_j_used && !g_capture_key && g_game_hwnd &&
                 GetForegroundWindow() == g_game_hwnd;
    if (g_anchor_any) {
        for (int i = 0; i < g_key_list_n; i++) {
            int vk = g_key_list[i];
            bool k = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (ready && k && !g_any_prev[vk]) {
                g_any_prev[vk] = true;
                do_anchor_now();
                break;
            }
            g_any_prev[vk] = k;
        }
    } else {
        bool k = (GetAsyncKeyState(g_anchor_vk) & 0x8000) != 0;
        if (ready && k && !g_j_prev) do_anchor_now();
        g_j_prev = k;
    }

    // 锚定后模拟按住键
    {
        bool fg = g_game_hwnd && GetForegroundWindow() == g_game_hwnd;
        bool want = g_hold_enabled && g_anchored && fg && g_hold_vk != 0;
        if (want && !g_holding) {
            keybd_event((BYTE)g_hold_vk, 0, 0, 0);
            g_holding = true;
            rflog("hold: down");
        } else if (!want && g_holding) {
            keybd_event((BYTE)g_hold_vk, 0, KEYEVENTF_KEYUP, 0);
            g_holding = false;
            rflog("hold: up");
        }
    }

    // 自动对齐：同步锚定/冻结状态
    static AaInfo s_aa;
    if (g_cfg.auto_align) {
        aa_poll(s_aa);
        if (s_aa.anchored) {
            if (!g_anchored) g_j_used = true;
            g_anchored = true;
            g_song_time = s_aa.song_time;
            g_stamp = s_aa.stamp;
        }
    }

    // 对拍校准：任意普通键边沿 → 记录与最近音符的时差
    if (g_calib && g_anchored && g_game_hwnd &&
        GetForegroundWindow() == g_game_hwnd && !g_capture_key) {
        for (int i = 0; i < g_key_list_n; i++) {
            int vk = g_key_list[i];
            bool k = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (k && !g_any_prev[vk]) {
                double d = g_song_time + (GetTickCount64() / 1000.0 - g_stamp) +
                           g_cfg.offset_ms / 1000.0;
                double best = 1e18;
                for (const ChartNote& n : g_chart.notes) {
                    double df = d - (double)n.t;
                    if (df < 0) df = -df;
                    if (df < best) best = df;
                }
                // 带符号：最近音符时间 = d ± best
                double nearest = 0;
                for (const ChartNote& n : g_chart.notes) {
                    if (fabs((double)n.t - (d - best)) < 1e-6) { nearest = n.t; break; }
                    if (fabs((double)n.t - (d + best)) < 1e-6) { nearest = n.t; break; }
                }
                g_calib_diffs.push_back(d - nearest);
                if (g_calib_diffs.size() >= 24) g_calib = false;
                break;
            }
            g_any_prev[vk] = k;
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(W, H);
    io.DeltaTime = 1.0f / 60.0f;
    g_menu_visible_for_input = g_menu_open;
    if (g_game_hwnd && g_menu_visible_for_input) {
        POINT pt;
        if (GetCursorPos(&pt) && ScreenToClient(g_game_hwnd, &pt)) {
            io.AddMousePosEvent((float)pt.x * g_dpi_scale,
                                (float)pt.y * g_dpi_scale);
            io.AddMouseButtonEvent(
                0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
            io.AddMouseButtonEvent(
                1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        }
    }
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    static bool ins_prev = false;
    {
        bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (ins && !ins_prev) g_menu_open = !g_menu_open;
        ins_prev = ins;
    }

    const float scale = g_cfg.scale_pct / 100.0f;
    float lane_w = g_cfg.lane_w_frac * H * scale;
    float gap = g_cfg.gap_frac * H * scale;
    float total = 4 * lane_w + 3 * gap;
    float x0 = (W - total) / 2;
    float judge_y = H * (1.0f - g_cfg.judge_frac) + g_cfg.judge_off_frac * H;
    float speed = g_cfg.speed_frac * H;
    float ball_d = g_cfg.note_frac * lane_w;
    float hold_w = 0.52f * lane_w;
    float jw = H * 0.004f;
    if (jw < 3) jw = 3;
    float lane_off = g_cfg.lane_off_frac * H;
    double disp = g_anchored
                      ? g_song_time + (now - g_stamp) + g_cfg.offset_ms / 1000.0
                      : 0.0;

    float centers[4];
    for (int i = 0; i < 4; i++)
        centers[i] = x0 + i * (lane_w + gap) + lane_w / 2 + lane_off;
    auto lane_c = [&](int lane) { return centers[g_cfg.mirror ? 3 - lane : lane]; };

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (g_cfg.visible) {
        if (g_cfg.dark_on) {
            int da = (int)(g_cfg.dark_a * 255);
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H),
                              IM_COL32(0, 0, 0, da));
        }
        ImU32 lcol = rgbs(g_cfg.lane_color, g_cfg.lane_a);
        ImU32 ecol = IM_COL32(0, 0, 0, 235);
        for (int i = 0; i < 4 && g_cfg.lane_vis; i++) {
            float lx = centers[i] - lane_w / 2;
            dl->AddRectFilled(ImVec2(lx, 0), ImVec2(lx + lane_w, judge_y),
                              lcol);
            dl->AddRectFilled(ImVec2(lx, 0), ImVec2(lx + 1.5f, judge_y),
                              ecol);
            dl->AddRectFilled(ImVec2(lx + lane_w - 1.5f, 0),
                              ImVec2(lx + lane_w, judge_y), ecol);
        }
        dl->AddRectFilled(ImVec2(x0 - 8, judge_y - jw / 2),
                          ImVec2(x0 + total + 8, judge_y + jw / 2),
                          rgbs(g_cfg.judge_color, 0.95f));

        if (g_anchored) {
            float r = ball_d / 2;
            for (const ChartNote& n : g_chart.notes) {
                if (n.lane < 0 || n.lane > 3) continue;
                float cx = lane_c(n.lane);
                if (n.end > 0) {
                    float lead = n.t - (float)disp;
                    float tlead = n.end - (float)disp;
                    if (tlead < -0.3f || lead > (judge_y / speed) + 0.5f)
                        continue;
                    float yh = judge_y - lead * speed;
                    float yt = judge_y - tlead * speed;
                    if (yh > judge_y) yh = judge_y;
                    if (yh < -ball_d) continue;
                    float top = yt + ball_d * 0.42f, bot = yh;
                    draw_hold_body(dl, cx, top, bot, hold_w, g_cfg);
                    if (yt > -ball_d && yt < H) {
                        float ta = (g_cfg.hold_a + 0.12f > 1.0f)
                                       ? 1.0f
                                       : g_cfg.hold_a + 0.12f;
                        if (g_cfg.hold_tail == 0)
                            dl->AddCircleFilled(
                                ImVec2(cx, yt + ball_d * 0.21f),
                                hold_w * 0.42f,
                                rgbs(g_cfg.hold_color, ta));
                        else if (g_cfg.hold_tail == 1)
                            dl->AddCircle(
                                ImVec2(cx, yt + ball_d * 0.21f),
                                hold_w * 0.42f,
                                rgbs(g_cfg.hold_color, ta < 0.6f ? 0.6f : ta),
                                0, 3.0f);
                    }
                    float a = (yh < 40) ? yh / 40 : 1.0f;
                    draw_tap(dl, cx, yh, r, a, g_cfg);
                } else {
                    float lead = n.t - (float)disp;
                    float y = judge_y - lead * speed;
                    if (y < -ball_d * 0.6f || y > judge_y + 90) continue;
                    float a = 1.0f;
                    if (y < 40) a = y / 40;
                    if (y > judge_y) a *= 1.0f - (y - judge_y) / 90.0f;
                    if (a <= 0.02f) continue;
                    draw_tap(dl, cx, y, r, a, g_cfg);
                }
            }
        }
    }

    // ===== INS 菜单 =====
    if (g_menu_open) {
        ImGui::SetNextWindowSize(ImVec2(680, 780), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
        ImGui::Begin("RizFalling · 落线  [INS 关闭]", &g_menu_open);
        if (g_loaded) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                               "%s  [%s] · %d 音符 · %s",
                               g_osz_name.c_str(), g_chart.diff.c_str(),
                               (int)g_chart.notes.size(),
                               g_anchored ? "已锚定" : "等待锚定");
            char kn[64];
            key_name_utf8(g_anchor_vk, kn, sizeof(kn));
            char mlab[160];
            if (g_anchor_any)
                wsprintfA(mlab, "锚定方式: 任意普通键（点击切换）");
            else
                wsprintfA(mlab, "锚定方式: 指定键 %s（点击切换）", kn);
            if (ImGui::Button(mlab)) {
                g_anchor_any = !g_anchor_any;
                g_cfg_dirty = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(g_anchor_any != 0 || g_capture_key);
            if (ImGui::Button(g_capture_key ? "请按键…" : "改键…")) {
                if (!g_capture_key) {
                    for (int vk = 0; vk < 256; vk++)
                        g_key_prev[vk] =
                            (GetAsyncKeyState(vk) & 0x8000) != 0;
                    g_capture_key = true;
                    g_capture_target = 0;
                    set_msg("请按一个新键（字母/数字/符号）");
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("指定键: %s", kn);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "未加载谱面");
        }
        {
            bool hv = g_hold_enabled != 0;
            if (ImGui::Checkbox("锚定后模拟按住按键", &hv)) {
                g_hold_enabled = hv ? 1 : 0;
                g_cfg_dirty = true;
            }
            ImGui::SameLine();
            char hk[64] = "未设置";
            if (g_hold_vk) key_name_utf8(g_hold_vk, hk, sizeof(hk));
            char hlab[128];
            if (g_capture_key && g_capture_target == 1)
                wsprintfA(hlab, "请按键…");
            else
                wsprintfA(hlab, "按住键: %s", hk);
            ImGui::BeginDisabled(!g_hold_enabled || g_capture_key);
            if (ImGui::Button(hlab)) {
                if (!g_capture_key) {
                    for (int vk = 0; vk < 256; vk++)
                        g_key_prev[vk] =
                            (GetAsyncKeyState(vk) & 0x8000) != 0;
                    g_capture_key = true;
                    g_capture_target = 1;
                    set_msg("请按要模拟按住的键（字母/数字/符号）");
                }
            }
            ImGui::EndDisabled();
        }
        {
            bool av = g_cfg.auto_align != 0;
            if (ImGui::Checkbox("自动对齐（音频匹配歌头）", &av)) {
                g_cfg.auto_align = av ? 1 : 0;
                g_cfg_dirty = true;
                aa_enable(av);
                if (av) g_anchored = false;   // 等自动锁定
            }
            if (g_cfg.auto_align && g_loaded) {
                ImGui::SameLine();
                ImGui::TextDisabled("<- %s", s_aa.status);
            }
        }
        {
            char clab[96];
            if (g_calib)
                wsprintfA(clab, "结束校准（已 %d 拍）",
                          (int)g_calib_diffs.size());
            else
                wsprintfA(clab, "对拍校准…");
            if (ImGui::Button(clab)) {
                if (!g_calib) {
                    if (!g_anchored) {
                        set_msg("先锚定再校准");
                    } else {
                        g_calib = true;
                        g_calib_diffs.clear();
                        set_msg("跟随音符节奏按任意键打拍（至少 6 下）");
                    }
                } else {
                    g_calib = false;
                    if (g_calib_diffs.size() >= 6) {
                        std::vector<double> t = g_calib_diffs;
                        std::sort(t.begin(), t.end());
                        double m = t[t.size() / 2];
                        double off = g_cfg.offset_ms - m * 1000.0;
                        if (off > 500) off = 500;
                        if (off < -500) off = -500;
                        g_cfg.offset_ms = (int)off;
                        g_cfg_dirty = true;
                        char b[128];
                        snprintf(b, sizeof(b), "校准完成：偏移 %d ms（中位 %.0f ms，%zu 拍）",
                                 g_cfg.offset_ms, m * 1000.0, g_calib_diffs.size());
                        set_msg(b);
                    } else {
                        set_msg("拍数不足，未调整");
                    }
                }
            }
            if (g_calib && !g_calib_diffs.empty()) {
                ImGui::SameLine();
                std::vector<double> t = g_calib_diffs;
                std::sort(t.begin(), t.end());
                ImGui::TextDisabled("中位 %+.0f ms", t[t.size() / 2] * 1000.0);
            }
        }
        if (g_msg[0] && now - g_msg_t < 6.0)
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", g_msg);
        ImGui::Separator();
        if (ImGui::Button("加载谱面…", ImVec2(140, 0))) {
            g_show_charts = !g_show_charts;
            if (g_show_charts &&
                (g_osz_files.empty() ||
                 GetTickCount64() / 1000.0 - g_osz_scan_t > 5))
                scan_osz_files();
        }
        ImGui::SameLine();
        if (ImGui::Button("谱面文件夹…")) {
            if (!g_dir_cs_init) {
                InitializeCriticalSection(&g_dir_cs);
                g_dir_cs_init = true;
            }
            CreateThread(nullptr, 0, pick_folder_proc, (LPVOID)g_game_hwnd,
                         0, nullptr);
        }
        ImGui::SameLine();
        {
            char kn[64], lab[96];
            key_name_utf8(g_anchor_vk, kn, sizeof(kn));
            wsprintfA(lab, "锚定 (%s)", kn);
            if (ImGui::Button(lab)) do_anchor_now();
        }
        ImGui::SameLine();
        if (ImGui::Button("重载谱面")) {
            if (g_loaded) load_chart_at(g_osz_name, g_diff_idx);
        }
        ImGui::SameLine();
        if (ImGui::Button("下一难度")) {
            if (g_loaded && g_diff_count > 0)
                load_chart_at(g_osz_name, (g_diff_idx + 1) % g_diff_count);
        }
        ImGui::SameLine();
        if (ImGui::Button("隐藏/显示")) g_cfg.visible = !g_cfg.visible;

        ImGui::Separator();
        int sp = (int)(g_cfg.speed_frac * H);
        if (ImGui::SliderInt("流速 px/s", &sp, 100, 2500)) {
            g_cfg.speed_frac = sp / H;
            g_cfg_dirty = true;
        }
        if (ImGui::SliderInt("偏移 ms", &g_cfg.offset_ms, -500, 500))
            g_cfg_dirty = true;
        int lo = (int)(g_cfg.lane_off_frac * H);
        if (ImGui::SliderInt("轨道水平偏移", &lo, -400, 400)) {
            g_cfg.lane_off_frac = lo / H;
            g_cfg_dirty = true;
        }
        int jo = (int)(g_cfg.judge_off_frac * H);
        if (ImGui::SliderInt("判定线高度", &jo, -400, 400)) {
            g_cfg.judge_off_frac = jo / H;
            g_cfg_dirty = true;
        }
        int lw = (int)(g_cfg.lane_w_frac * H);
        if (ImGui::SliderInt("轨道宽度", &lw, 40, 220)) {
            g_cfg.lane_w_frac = lw / H;
            g_cfg_dirty = true;
        }
        int nb = (int)(g_cfg.note_frac * 100);
        if (ImGui::SliderInt("球大小 %", &nb, 30, 120)) {
            g_cfg.note_frac = nb / 100.0f;
            g_cfg_dirty = true;
        }
        int sc = g_cfg.scale_pct;
        if (ImGui::SliderInt("谱面缩放 %", &sc, 50, 200)) {
            g_cfg.scale_pct = sc;
            g_cfg_dirty = true;
        }
        {
            bool lv = g_cfg.lane_vis != 0;
            if (ImGui::Checkbox("显示轨道", &lv)) {
                g_cfg.lane_vis = lv ? 1 : 0;
                g_cfg_dirty = true;
            }
            ImGui::SameLine();
            bool mv = g_cfg.mirror != 0;
            if (ImGui::Checkbox("镜像（左手）", &mv)) {
                g_cfg.mirror = mv ? 1 : 0;
                g_cfg_dirty = true;
            }
        }
        {
            const char* tails[] = {"尖尾 ●", "空缺 ○", "无"};
            int tt = g_cfg.hold_tail;
            ImGui::SetNextItemWidth(160);
            if (ImGui::Combo("投皮尾形", &tt, tails, 3)) {
                g_cfg.hold_tail = tt;
                g_cfg_dirty = true;
            }
        }
        {
            const char* taps[] = {"圆球 ●", "圆环 ◯", "方块 ■", "菱形 ◆"};
            int ts = g_cfg.tap_skin;
            ImGui::SetNextItemWidth(160);
            if (ImGui::Combo("球皮", &ts, taps, 4)) {
                g_cfg.tap_skin = ts;
                g_cfg_dirty = true;
            }
            ImGui::SameLine();
            const char* holds[] = {"实心条", "描边条", "管道"};
            int hs = g_cfg.hold_skin;
            ImGui::SetNextItemWidth(160);
            if (ImGui::Combo("砖皮", &hs, holds, 3)) {
                g_cfg.hold_skin = hs;
                g_cfg_dirty = true;
            }
        }
        {
            float col[3] = {((g_cfg.lane_color >> 16) & 0xFF) / 255.0f,
                            ((g_cfg.lane_color >> 8) & 0xFF) / 255.0f,
                            (g_cfg.lane_color & 0xFF) / 255.0f};
            if (ImGui::ColorEdit3("轨道颜色", col)) {
                g_cfg.lane_color = ((int)(col[0] * 255) << 16) |
                                   ((int)(col[1] * 255) << 8) |
                                   (int)(col[2] * 255);
                g_cfg_dirty = true;
            }
        }
        int la = (int)(g_cfg.lane_a * 255);
        if (ImGui::SliderInt("轨道不透明度", &la, 0, 255)) {
            g_cfg.lane_a = la / 255.0f;
            g_cfg_dirty = true;
        }
        {
            bool dv = g_cfg.dark_on != 0;
            if (ImGui::Checkbox("全屏暗色（读谱）", &dv)) {
                g_cfg.dark_on = dv ? 1 : 0;
                g_cfg_dirty = true;
            }
        }
        int da = (int)(g_cfg.dark_a * 255);
        if (ImGui::SliderInt("暗色强度", &da, 0, 220)) {
            g_cfg.dark_a = da / 255.0f;
            g_cfg_dirty = true;
        }
        ImGui::Separator();
        ImGui::Text("音符: %d    %.0f FPS", (int)g_chart.notes.size(),
                    io.Framerate);
        ImGui::End();
    }

    // ===== 谱面列表 =====
    if (g_show_charts) {
        ImGui::SetNextWindowSize(ImVec2(700, 520), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(730, 120), ImGuiCond_FirstUseEver);
        ImGui::Begin("选择谱面 (.osz)", &g_show_charts);
        {
            char du[1024] = {0};
            if (g_osz_dir.empty()) {
                strcpy(du, "未选择（点「换文件夹…」）");
            } else {
                WideCharToMultiByte(CP_UTF8, 0, g_osz_dir.c_str(), -1, du,
                                    sizeof(du) - 1, nullptr, nullptr);
            }
            ImGui::TextWrapped("文件夹: %s", du);
            ImGui::SameLine();
            if (ImGui::SmallButton("换文件夹…")) {
                if (!g_dir_cs_init) {
                    InitializeCriticalSection(&g_dir_cs);
                    g_dir_cs_init = true;
                }
                CreateThread(nullptr, 0, pick_folder_proc,
                             (LPVOID)g_game_hwnd, 0, nullptr);
            }
        }
        static char osz_filter[64] = "";
        static int page = 0;
        const int PER = 16;
        if (ImGui::Button("刷新")) {
            scan_osz_files();
            page = 0;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(230);
        if (ImGui::InputTextWithHint("##filter", "搜索曲名…", osz_filter, 64))
            page = 0;
        ImGui::SameLine();
        if (ImGui::Button("上一页") && page > 0) page--;
        ImGui::SameLine();
        if (ImGui::Button("下一页")) page++;
        std::vector<std::string> list;
        for (auto& f : g_osz_files) {
            if (osz_filter[0]) {
                std::string lf = f, lq = osz_filter;
                for (auto& ch : lf)
                    ch = (char)tolower((unsigned char)ch);
                for (auto& ch : lq)
                    ch = (char)tolower((unsigned char)ch);
                if (lf.find(lq) == std::string::npos) continue;
            }
            list.push_back(f);
        }
        int pages = ((int)list.size() + PER - 1) / PER;
        if (pages < 1) pages = 1;
        if (page >= pages) page = pages - 1;
        if (page < 0) page = 0;
        ImGui::Text("共 %d 首 · 第 %d/%d 页 · IN/HD/EZ 加载，其它难度用「下一难度」",
                    (int)list.size(), page + 1, pages);
        ImGui::Separator();
        ImGui::BeginChild("##oszlist", ImVec2(0, 400), true);
        int start = page * PER;
        int end = (int)list.size();
        if (end > start + PER) end = start + PER;
        for (int i = start; i < end; i++) {
            const std::string& f = list[i];
            ImGui::PushID(f.c_str());
            ImGui::TextUnformatted(f.c_str());
            ImGui::SameLine(450);
            if (ImGui::SmallButton("IN")) {
                load_chart_at(f, 0);
                g_show_charts = false;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("HD")) {
                load_chart_at(f, 1);
                g_show_charts = false;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("EZ")) {
                load_chart_at(f, 2);
                g_show_charts = false;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::End();
    }

    if (g_cfg_dirty && now - g_cfg_save_t > 0.8) {
        cfg_save();
        g_cfg_dirty = false;
        g_cfg_save_t = now;
    }

    ImGui::Render();
    if (!g_rtv || g_rtv_tex != bb) {
        if (g_rtv) {
            g_rtv->Release();
            g_rtv = nullptr;
        }
        if (FAILED(dev->CreateRenderTargetView(bb, nullptr, &g_rtv))) return;
        g_rtv_tex = bb;
    }
    ID3D11RenderTargetView* rtv = g_rtv;
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

// ---------------- Present hook ----------------
static const GUID IID_D3D11Device =
    {0xdb6f6ddb, 0xac77, 0x4e88, {0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40}};
static const GUID IID_D3D11Texture2D =
    {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT,
                                                      UINT, UINT, DXGI_FORMAT,
                                                      UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(void*, UINT, UINT,
                                                 const void*);
static PFN_Present g_orig_present = nullptr;
static PFN_ResizeBuffers g_orig_resize = nullptr;
static PFN_Present1 g_orig_present1 = nullptr;

static void render_on(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(IID_D3D11Device, (void**)&dev)) && dev) {
        ID3D11DeviceContext* ctx = nullptr;
        dev->GetImmediateContext(&ctx);
        ID3D11Texture2D* bb = nullptr;
        DXGI_SWAP_CHAIN_DESC scd = {};
        sc->GetDesc(&scd);
        if (ctx &&
            SUCCEEDED(sc->GetBuffer(0, IID_D3D11Texture2D, (void**)&bb))) {
            draw_overlay(dev, ctx, bb, scd.OutputWindow);
            bb->Release();
        }
        if (ctx) ctx->Release();
        dev->Release();
    }
}

static HRESULT hook_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    render_on(sc);
    return g_orig_present(sc, sync, flags);
}

static HRESULT hook_resize(IDXGISwapChain* sc, UINT bc, UINT w, UINT h,
                           DXGI_FORMAT f, UINT fl) {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
        g_rtv_tex = nullptr;
    }
    return g_orig_resize(sc, bc, w, h, f, fl);
}

static HRESULT hook_present1(void* sc, UINT sync, UINT flags, const void* pp) {
    render_on(reinterpret_cast<IDXGISwapChain*>(sc));
    return g_orig_present1(sc, sync, flags, pp);
}

static void patch_slot(void** vt, int idx, void* hook, void** orig) {
    DWORD old;
    if (VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        *orig = (void*)InterlockedExchangePointer((PVOID volatile*)&vt[idx],
                                                  hook);
        VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    }
}

static void patch_vtable(void* swapchain) {
    void** vt = *reinterpret_cast<void***>(swapchain);
    if (!g_orig_present)
        patch_slot(vt, 8, (void*)&hook_present, (void**)&g_orig_present);
    if (!g_orig_resize)
        patch_slot(vt, 13, (void*)&hook_resize, (void**)&g_orig_resize);
    if (!g_orig_present1) {
        void* cand = vt[22];
        if (cand && cand != vt[8] && cand != vt[13])
            patch_slot(vt, 22, (void*)&hook_present1, (void**)&g_orig_present1);
    }
}

static LRESULT CALLBACK dummy_proc(HWND w, UINT m, WPARAM a, LPARAM b) {
    return DefWindowProcW(w, m, a, b);
}

void ensure_hook() {
    static LONG busy = 0;
    if (g_orig_present || InterlockedCompareExchange(&busy, 1, 0) != 0) return;
    rflog("hook: enter");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = dummy_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RfDummyWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 128, 128, nullptr, nullptr, wc.hInstance,
                                nullptr);
    rflog(hwnd ? "hook: dummy window ok" : "hook: dummy window FAILED");
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 128;
    sd.BufferDesc.Height = 128;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    using Fn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                const D3D_FEATURE_LEVEL*, UINT, UINT,
                                const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                ID3D11Device**, D3D_FEATURE_LEVEL*,
                                ID3D11DeviceContext**);
    HMODULE sys = LoadLibraryW(L"C:\\Windows\\System32\\d3d11.dll");
    auto fn = sys ? reinterpret_cast<Fn>(
                        GetProcAddress(sys, "D3D11CreateDeviceAndSwapChain"))
                  : nullptr;
    rflog(fn ? "hook: real fn ok" : "hook: real fn FAILED");
    if (fn) {
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        IDXGISwapChain* sc = nullptr;
        D3D_FEATURE_LEVEL fl;
        if (SUCCEEDED(fn(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                         nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl,
                         &ctx)) &&
            sc) {
            patch_vtable(sc);
            sc->Release();
            rflog("hook: patched present");
        } else {
            rflog("hook: dummy device creation FAILED");
        }
        if (ctx) ctx->Release();
        if (dev) dev->Release();
    }
}
