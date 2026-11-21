// Hangar - Direct2D / DirectWrite helpers and the theme
#include "gfx.h"
#include "util.h"
#include <dwmapi.h>
#include <map>
#include <tuple>
#include <unordered_map>

namespace gfx {

ID2D1Factory* d2d = nullptr;
IDWriteFactory* dw = nullptr;
static std::wstring s_iconFont = L"Segoe MDL2 Assets";
static std::wstring s_uiFont = L"Segoe UI";

static bool FontExists(IDWriteFontCollection* sys, const wchar_t* name) {
    UINT32 idx;
    BOOL exists = FALSE;
    return sys && SUCCEEDED(sys->FindFamilyName(name, &idx, &exists)) && exists;
}

bool Init() {
    if (d2d && dw) return true;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&dw));
    if (!d2d || !dw) return false;
    IDWriteFontCollection* sys = nullptr;
    dw->GetSystemFontCollection(&sys, FALSE);
    if (FontExists(sys, L"Segoe Fluent Icons")) s_iconFont = L"Segoe Fluent Icons";
    if (FontExists(sys, L"Segoe UI Variable Text")) s_uiFont = L"Segoe UI Variable Text";
    else if (!FontExists(sys, L"Segoe UI") && FontExists(sys, L"Tahoma")) s_uiFont = L"Tahoma";   // Wine test runs
    SafeRelease(sys);
    return true;
}

const wchar_t* IconFont() { return s_iconFont.c_str(); }
const wchar_t* UiFont() { return s_uiFont.c_str(); }

IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align, bool wrap, bool icon) {
    static std::map<std::tuple<int, int, int, bool, bool>, IDWriteTextFormat*> cache;
    auto key = std::make_tuple((int)(size * 10), (int)weight, (int)align, wrap, icon);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    dw->CreateTextFormat(icon ? s_iconFont.c_str() : s_uiFont.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (f) {
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap) {
            IDWriteInlineObject* ell = nullptr;
            dw->CreateEllipsisTrimmingSign(f, &ell);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            f->SetTrimming(&trim, ell);
            SafeRelease(ell);
        } else {
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            f->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size * 1.42f, size * 1.12f);
        }
    }
    cache[key] = f;
    return f;
}

// ---------------------------------------------------------------- text caches
struct LayoutKey {
    IDWriteTextFormat* f;
    int w, h;   // in quarter DIPs
    std::wstring s;
    bool operator==(const LayoutKey& o) const { return f == o.f && w == o.w && h == o.h && s == o.s; }
};
struct LayoutKeyHash {
    size_t operator()(const LayoutKey& k) const {
        size_t x = std::hash<std::wstring>()(k.s);
        x ^= std::hash<void*>()(k.f) + 0x9e3779b9 + (x << 6) + (x >> 2);
        x ^= (size_t)k.w * 1315423911u ^ (size_t)k.h * 2654435761u;
        return x;
    }
};
struct LayoutEntry { IDWriteTextLayout* layout; unsigned lastFrame; };
static std::unordered_map<LayoutKey, LayoutEntry, LayoutKeyHash> s_layouts;
static std::unordered_map<LayoutKey, float, LayoutKeyHash> s_widths, s_heights;
static unsigned s_frame = 0;

IDWriteTextLayout* Layout(const std::wstring& s, IDWriteTextFormat* f, float w, float h) {
    if (!f || s.empty() || w <= 0 || h <= 0) return nullptr;
    LayoutKey k{f, (int)(w * 4 + 0.5f), (int)(h * 4 + 0.5f), s};
    auto it = s_layouts.find(k);
    if (it != s_layouts.end()) { it->second.lastFrame = s_frame; return it->second.layout; }
    IDWriteTextLayout* l = nullptr;
    if (FAILED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, k.w / 4.0f, k.h / 4.0f, &l))) return nullptr;
    s_layouts.emplace(std::move(k), LayoutEntry{l, s_frame});
    return l;
}

void FrameDone() {
    ++s_frame;
    // a long window resize makes new sizes every frame; don't let that pile up
    if (s_layouts.size() > 12000) { ClearTextCaches(); return; }
    // every ~4 seconds of frames, let go of layouts nobody drew lately (scrolled away, old text)
    if (s_frame % 240 != 0 && s_layouts.size() < 6000) return;
    for (auto it = s_layouts.begin(); it != s_layouts.end();) {
        if (s_frame - it->second.lastFrame > 240) { it->second.layout->Release(); it = s_layouts.erase(it); }
        else ++it;
    }
    if (s_widths.size() > 4000) s_widths.clear();
    if (s_heights.size() > 2000) s_heights.clear();
}

void ClearTextCaches() {
    for (auto& kv : s_layouts) kv.second.layout->Release();
    s_layouts.clear();
    s_widths.clear();
    s_heights.clear();
}

void Canvas::Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, D2D1_COLOR_F c) {
    if (!f || s.empty()) return;
    float w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0.5f || h <= 0.5f) return;
    IDWriteTextLayout* l = Layout(s, f, w, h);
    if (!l) return;
    br->SetColor(c);
    rt->DrawTextLayout(D2D1::Point2F(r.left, r.top), l, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float MeasureWidth(const std::wstring& s, IDWriteTextFormat* f) {
    if (!f || s.empty()) return 0;
    LayoutKey k{f, 0, 0, s};
    auto it = s_widths.find(k);
    if (it != s_widths.end()) return it->second;
    IDWriteTextLayout* l = nullptr;
    float w = 0;
    if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000.f, 1000.f, &l))) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        l->Release();
    }
    s_widths.emplace(std::move(k), w);
    return w;
}

float MeasureHeight(const std::wstring& s, IDWriteTextFormat* f, float width) {
    if (!f || s.empty()) return 0;
    LayoutKey k{f, (int)(width * 4 + 0.5f), 0, s};
    auto it = s_heights.find(k);
    if (it != s_heights.end()) return it->second;
    IDWriteTextLayout* l = nullptr;
    float h = 0;
    if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, width, 100000.f, &l))) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->GetMetrics(&m))) h = m.height;
        l->Release();
    }
    s_heights.emplace(std::move(k), h);
    return h;
}

// ---------------------------------------------------------------- theme
static int s_light = -1;
static int64_t s_accent = -1;

void InvalidateSystemColors() { s_light = -1; s_accent = -1; }

static bool ReadSystemUsesLightTheme() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return false;
    return v != 0;
}

bool SystemUsesLightTheme() {
    if (s_light < 0) s_light = ReadSystemUsesLightTheme() ? 1 : 0;
    return s_light == 1;
}

static uint32_t ReadSystemAccent() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) {
        // stored as 0xAABBGGRR
        return ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
    }
    return 0x4F8CFF;
}

uint32_t SystemAccent() {
    if (s_accent < 0) s_accent = ReadSystemAccent();
    return (uint32_t)s_accent;
}

bool BackdropSupported() {
    static int s = -1;
    if (s < 0) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        OSVERSIONINFOW vi{sizeof(vi)};
        s = (fn && fn(&vi) == 0 && vi.dwBuildNumber >= 22621) ? 1 : 0;   // Windows 11 22H2
        // Wine reports a Windows version but has no DWM backdrops
        if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) s = 0;
    }
    return s == 1;
}

float Luma(D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

void ApplyWindowTheme(HWND hwnd, const Theme& t) {
    BOOL dark = t.dark;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int corner = 2; /*DWMWCP_ROUND*/
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    if (BackdropSupported()) {
        int type = t.backdrop ? t.backdropType : 1 /*DWMSBT_NONE*/;
        DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &type, sizeof(type));
        MARGINS m = t.backdrop ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(hwnd, &m);
    }
    // a solid title bar that matches the theme (Midnight gets a black one); default over a backdrop
    COLORREF cap = t.backdrop ? 0xFFFFFFFF /*DWMWA_COLOR_DEFAULT*/
                              : RGB((t.captionRgb >> 16) & 0xFF, (t.captionRgb >> 8) & 0xFF, t.captionRgb & 0xFF);
    DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
}

} // namespace gfx
