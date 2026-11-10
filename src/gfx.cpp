// Hangar - Direct2D / DirectWrite helpers and the theme
#include "gfx.h"
#include "util.h"
#include <dwmapi.h>
#include <map>
#include <tuple>

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

float MeasureWidth(const std::wstring& s, IDWriteTextFormat* f) {
    if (!f || s.empty()) return 0;
    IDWriteTextLayout* l = nullptr;
    float w = 0;
    if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 10000.f, 1000.f, &l))) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
        l->Release();
    }
    return w;
}

float MeasureHeight(const std::wstring& s, IDWriteTextFormat* f, float width) {
    if (!f || s.empty()) return 0;
    IDWriteTextLayout* l = nullptr;
    float h = 0;
    if (SUCCEEDED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, width, 100000.f, &l))) {
        DWRITE_TEXT_METRICS m;
        if (SUCCEEDED(l->GetMetrics(&m))) h = m.height;
        l->Release();
    }
    return h;
}

// ---------------------------------------------------------------- theme
bool SystemUsesLightTheme() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return false;
    return v != 0;
}

uint32_t SystemAccent() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) {
        // stored as 0xAABBGGRR
        return ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
    }
    return 0x4F8CFF;
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

static float Luma(D2D1_COLOR_F c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

Theme BuildTheme(const std::wstring& name) {
    Theme t;
    t.dark = name == L"light" ? false : name == L"system" ? !SystemUsesLightTheme() : true;
    t.backdrop = BackdropSupported();
    D2D1_COLOR_F accent = Rgb(SystemAccent());
    // keep the accent readable on both backgrounds
    if (t.dark && Luma(accent) < 0.30f) accent = Mix(accent, Rgb(0xFFFFFF), 0.35f);
    if (!t.dark && Luma(accent) > 0.55f) accent = Mix(accent, Rgb(0x000000), 0.30f);
    t.accent = accent;
    t.accentHover = t.dark ? Mix(accent, Rgb(0xFFFFFF), 0.12f) : Mix(accent, Rgb(0x000000), 0.10f);
    t.onAccent = Luma(accent) > 0.55f ? Rgb(0x101014) : Rgb(0xFFFFFF);
    float bgA = t.backdrop ? 0.80f : 1.0f;
    if (t.dark) {
        t.bg = Rgb(0x16161A, bgA);
        t.sidebar = Rgb(0x111114, t.backdrop ? 0.55f : 1.0f);
        t.surface = Rgb(0x222228);
        t.surfaceHover = Rgb(0x2A2A31);
        t.surfaceStrong = Rgb(0x34343C);
        t.divider = Rgb(0xFFFFFF, 0.08f);
        t.text = Rgb(0xF2F2F5);
        t.subtext = Rgb(0xA8A8B3);
        t.faint = Rgb(0x6E6E7A);
        t.good = Rgb(0x5BD18B);
        t.warn = Rgb(0xF2B84B);
        t.bad = Rgb(0xFF6B6B);
        t.info = Rgb(0x6CB4FF);
    } else {
        t.bg = Rgb(0xF5F5F7, bgA);
        t.sidebar = Rgb(0xEBEBEF, t.backdrop ? 0.55f : 1.0f);
        t.surface = Rgb(0xFFFFFF);
        t.surfaceHover = Rgb(0xF7F7FA);
        t.surfaceStrong = Rgb(0xE6E6EC);
        t.divider = Rgb(0x000000, 0.08f);
        t.text = Rgb(0x16161A);
        t.subtext = Rgb(0x5C5C68);
        t.faint = Rgb(0x8E8E9A);
        t.good = Rgb(0x1F9D57);
        t.warn = Rgb(0xB7791F);
        t.bad = Rgb(0xD64545);
        t.info = Rgb(0x2F6FDB);
    }
    return t;
}

void ApplyWindowTheme(HWND hwnd, const Theme& t) {
    BOOL dark = t.dark;
    DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int corner = 2; /*DWMWCP_ROUND*/
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    if (BackdropSupported()) {
        int type = t.backdrop ? 2 /*DWMSBT_MAINWINDOW (Mica)*/ : 1 /*DWMSBT_NONE*/;
        DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &type, sizeof(type));
        MARGINS m = t.backdrop ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(hwnd, &m);
    } else {
        COLORREF cap = t.dark ? RGB(0x16, 0x16, 0x1A) : RGB(0xF5, 0xF5, 0xF7);
        DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
    }
}

} // namespace gfx
