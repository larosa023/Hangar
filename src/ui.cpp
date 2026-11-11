// Hangar - main window: library, app pages, settings. Drawn with Direct2D; all app state lives
// on this thread and background jobs report back with posted messages.
#include "ui.h"
#include "gfx.h"
#include "model.h"
#include "jobs.h"
#include "config.h"
#include "util.h"
#include "net.h"
#include "selfupdate.h"
#include "json.h"
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <memory>

using namespace gfx;

// ---------------------------------------------------------------- state
struct AppState {
    AppDef def;
    Installed inst;
    bool scanned = false;
    std::vector<ReleaseInfo> releases;
    bool releasesLoaded = false;
    bool checking = false;
    std::wstring releaseError;
    long long checkedAt = 0;
    std::vector<DWORD> pids;
    bool busy = false;
    OpKind op = OpKind::Install;
    int percent = -1;
    float shownPercent = 0;
    std::wstring opText;
    std::wstring lastError;
    std::vector<uint8_t> iconPixels;
    UINT iconW = 0, iconH = 0;
    ID2D1Bitmap* icon = nullptr;
    float hover = 0;
};

enum class Page { Library, Details, Settings };

enum class Hit {
    None, Nav, Search, SearchClear, ViewGrid, ViewList, Refresh, UpdateAll, Card, CardAction, Back,
    DAction, DCancel, DOpenFolder, DRepair, DUninstall, DLocate, DChannel, DReleasePage, DRepo,
    SetTheme, SetChannel, SetHours, SetCheckNow, SetOpenData, SetOpenLog, SetLocalManifest, SetRepo, ToastClose,
    SelfRestart, SelfCheck, SelfChannel, SelfAuto, SelfRollback, WhatsNewClose,
};

struct HotRect {
    D2D1_RECT_F r;
    Hit kind;
    int index;
    int arg;
    bool enabled;
};

static HWND g_hwnd;
static ID2D1HwndRenderTarget* g_rt;
static ID2D1SolidColorBrush* g_br;
static Theme g_t;
static float g_scale = 1.0f;
static float g_W = 0, g_H = 0;

static Manifest g_manifest;
static std::wstring g_manifestText;
static std::vector<AppState> g_apps;

static Page g_page = Page::Library;
static std::wstring g_nav = L"all";       // all | installed | updates | <category id>
static int g_detail = -1;
static std::wstring g_search;
static bool g_searchFocus = false;
static bool g_caretOn = true;
static int g_focus = -1;                   // keyboard focus in the library (index into visible list)
static bool g_keyboardMode = false;
static float g_scroll = 0, g_scrollTarget = 0, g_contentH = 0, g_viewH = 0;
static std::map<Page, float> g_savedScroll;
static int g_cols = 1;
static float g_pageAnim = 1.0f;

static std::vector<HotRect> g_hot;
static HotRect g_hover{{0, 0, 0, 0}, Hit::None, -1, 0, false};
static HotRect g_pressed{{0, 0, 0, 0}, Hit::None, -1, 0, false};
static bool g_tracking = false;

static std::wstring g_toast;
static bool g_toastBad = false;
static DWORD g_toastAt = 0;
static float g_toastAnim = 0;

static std::wstring g_manifestError;
static bool g_offline = false;
static DWORD g_lastScan = 0;
static std::vector<int> g_visible;          // indices into g_apps shown on the library page

enum : UINT_PTR { TIMER_ANIM = 1, TIMER_CARET, TIMER_PROCS, TIMER_CHECK, TIMER_TOAST, TIMER_SELF, TIMER_HEALTHY };
static bool g_animTimer = false;

// Hangar's own updater
static SelfUpdateState g_self;
static DWORD g_selfCheckedTick = 0;
static DWORD g_lastInput = 0;
static bool g_applyWhenIdle = false;
static std::wstring g_whatsNew;            // shown once after Hangar updates itself
static std::wstring g_whatsNewVersion;
static selfupdate::AfterUpdate g_after;

static void Invalidate() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }
static void StartAnim() {
    if (!g_animTimer && g_hwnd) { SetTimer(g_hwnd, TIMER_ANIM, 15, nullptr); g_animTimer = true; }
    Invalidate();
}

// ---------------------------------------------------------------- app helpers
static AppState* FindApp(const std::wstring& id) {
    for (auto& a : g_apps) if (a.def.id == id) return &a;
    return nullptr;
}
static bool UsesBeta(const AppState& a) { return AppUsesBeta(a.def.id); }
static const ReleaseInfo* Latest(const AppState& a) { return LatestRelease(a.releases, a.def, UsesBeta(a)); }
static bool NeedsNewerHangar(const AppState& a) { return !a.def.minHangar.empty() && CompareVersions(HANGAR_VERSION, a.def.minHangar) < 0; }
static bool Running(const AppState& a) { return !a.pids.empty(); }
static bool HasUpdate(const AppState& a) {
    if (!a.inst.installed || a.inst.version.empty() || a.busy) return false;
    const ReleaseInfo* l = Latest(a);
    return l && CompareVersions(l->version, a.inst.version) > 0;
}
static std::wstring CategoryName(const std::wstring& id) {
    for (auto& c : g_manifest.categories) if (c.id == id) return c.name;
    if (id.empty() || id == L"other") return L"Other";
    std::wstring s = id;
    s[0] = towupper(s[0]);
    return s;
}
static int UpdateCount() {
    int n = 0;
    for (auto& a : g_apps) if (HasUpdate(a)) ++n;
    return n;
}

struct StatusInfo { std::wstring label; D2D1_COLOR_F color; };

static StatusInfo Status(const AppState& a) {
    if (a.busy) {
        const wchar_t* verb = a.op == OpKind::Uninstall ? L"Removing" : a.op == OpKind::Update ? L"Updating" : a.op == OpKind::Repair ? L"Repairing" : L"Installing";
        return {verb, g_t.accent};
    }
    if (!a.lastError.empty()) return {L"Error", g_t.bad};
    if (NeedsNewerHangar(a)) return {L"Needs newer Hangar", g_t.faint};
    if (HasUpdate(a)) return {L"Update available", g_t.warn};
    if (a.inst.installed) return Running(a) ? StatusInfo{L"Running", g_t.good} : StatusInfo{L"Installed", g_t.info};
    if (!a.scanned) return {L"", g_t.faint};
    return {L"Not installed", g_t.faint};
}

static std::wstring VersionLine(const AppState& a) {
    const ReleaseInfo* l = Latest(a);
    if (a.inst.installed) {
        std::wstring v = a.inst.version.empty() ? L"Installed" : L"v" + a.inst.version;
        if (HasUpdate(a)) v += L"  \x2192  v" + l->version;
        return v;
    }
    if (l) return L"v" + l->version + (l->prerelease ? L" beta" : L"");
    if (a.checking || !a.releasesLoaded) return L"Checking…";
    if (!a.releaseError.empty()) return L"Couldn't check for releases";
    return L"No release yet";
}

enum class Action { None, Install, Update, Open, Cancel, Retry };

static Action PrimaryAction(const AppState& a, std::wstring* label, bool* enabled) {
    *enabled = true;
    if (a.busy) {
        *label = L"Cancel";
        *enabled = a.op != OpKind::Uninstall && a.percent >= 0;
        return Action::Cancel;
    }
    if (NeedsNewerHangar(a)) { *label = L"Unavailable"; *enabled = false; return Action::None; }
    if (HasUpdate(a)) { *label = L"Update"; return Action::Update; }
    if (a.inst.installed) { *label = L"Open"; return Action::Open; }
    *label = L"Install";
    *enabled = Latest(a) != nullptr;
    return Action::Install;
}

// ---------------------------------------------------------------- toasts
static void Toast(const std::wstring& text, bool bad = false) {
    g_toast = text;
    g_toastBad = bad;
    g_toastAt = GetTickCount();
    SetTimer(g_hwnd, TIMER_TOAST, bad ? 7000 : 4500, nullptr);
    StartAnim();
}

// ---------------------------------------------------------------- operations
static std::vector<std::wstring> RunningNames(const std::vector<int>& idx) {
    std::vector<std::wstring> out;
    for (int i : idx) if (Running(g_apps[i])) out.push_back(g_apps[i].def.name);
    return out;
}

static std::wstring JoinNames(const std::vector<std::wstring>& names) {
    std::wstring s;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) s += (i + 1 == names.size()) ? L" and " : L", ";
        s += names[i];
    }
    return s;
}

static void StartOp(int i, OpKind kind, const ReleaseInfo* rel) {
    AppState& a = g_apps[i];
    if (a.busy) return;
    OpRequest req;
    req.kind = kind;
    req.def = a.def;
    req.inst = a.inst;
    req.beta = UsesBeta(a);
    if (rel) req.release = *rel;
    a.busy = true;
    a.op = kind;
    a.percent = -1;
    a.shownPercent = 0;
    a.opText = kind == OpKind::Uninstall ? L"Starting…" : L"Preparing…";
    a.lastError.clear();
    jobs::StartOp(req);
    StartAnim();
}

static bool ConfirmClose(const std::vector<int>& idx) {
    auto names = RunningNames(idx);
    if (names.empty()) return true;
    std::wstring msg = JoinNames(names) + (names.size() == 1 ? L" is" : L" are") +
                       L" open. Hangar will close " + (names.size() == 1 ? L"it" : L"them") +
                       L", update, and open " + (names.size() == 1 ? L"it" : L"them") + L" again.\n\nSave your work first. Continue?";
    return MessageBoxW(g_hwnd, msg.c_str(), L"Hangar", MB_OKCANCEL | MB_ICONINFORMATION) == IDOK;
}

static void Launch(AppState& a) {
    if (!a.inst.installed || a.inst.exe.empty()) return;
    HINSTANCE r = ShellExecuteW(g_hwnd, L"open", a.inst.exe.c_str(), nullptr, a.inst.dir.c_str(), SW_SHOWNORMAL);
    if ((INT_PTR)r <= 32) Toast(L"Couldn't open " + a.def.name + L".", true);
    else SetTimer(g_hwnd, TIMER_PROCS, 800, nullptr);   // pick up "Running" quickly
}

static void UpdateAll() {
    std::vector<int> idx;
    for (size_t i = 0; i < g_apps.size(); ++i) if (HasUpdate(g_apps[i])) idx.push_back((int)i);
    if (idx.empty()) { Toast(L"Everything is up to date."); return; }
    if (!ConfirmClose(idx)) return;
    for (int i : idx) StartOp(i, OpKind::Update, Latest(g_apps[i]));
}

static void DoPrimary(int i) {
    AppState& a = g_apps[i];
    std::wstring label;
    bool enabled;
    Action act = PrimaryAction(a, &label, &enabled);
    if (!enabled) return;
    switch (act) {
        case Action::Install: StartOp(i, OpKind::Install, Latest(a)); break;
        case Action::Update:
            if (ConfirmClose({i})) StartOp(i, OpKind::Update, Latest(a));
            break;
        case Action::Open: Launch(a); break;
        case Action::Cancel: jobs::CancelOp(a.def.id); a.opText = L"Cancelling…"; Invalidate(); break;
        default: break;
    }
}

static void Repair(int i) {
    AppState& a = g_apps[i];
    const ReleaseInfo* rel = nullptr;
    for (auto& r : a.releases) if (!a.inst.version.empty() && CompareVersions(r.version, a.inst.version) == 0 && FindInstallAsset(r, a.def, r.prerelease)) rel = &r;
    if (!rel) rel = Latest(a);
    if (!rel) { Toast(L"No release to repair from.", true); return; }
    std::wstring msg = L"Download " + a.def.name + L" " + rel->version + L" again, verify it, and reinstall it?\n\nYour settings are kept.";
    if (MessageBoxW(g_hwnd, msg.c_str(), L"Repair", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    if (!ConfirmClose({i})) return;
    StartOp(i, OpKind::Repair, rel);
}

static void Uninstall(int i) {
    AppState& a = g_apps[i];
    bool selfUninstall = a.def.type == InstallType::Portable && !a.def.uninstallArg.empty();
    if (!selfUninstall) {
        std::wstring msg = L"Uninstall " + a.def.name + L"?";
        if (Running(a)) msg += L"\n\nIt's open right now. Hangar will close it first.";
        msg += a.def.type == InstallType::Portable ? L"\n\nYour settings are kept." : L"";
        if (MessageBoxW(g_hwnd, msg.c_str(), L"Uninstall", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    }
    StartOp(i, OpKind::Uninstall, nullptr);
}

// For a copy Hangar couldn't find by itself (say, an exe kept in a tools folder)
static void Locate(int i) {
    AppState& a = g_apps[i];
    wchar_t file[MAX_PATH * 2] = {};
    std::wstring filter = a.def.exe + L'\0' + a.def.exe + L'\0' + L"Programs (*.exe)" + L'\0' + L"*.exe" + L'\0' + L'\0';
    std::wstring title = L"Where is " + a.def.name + L"?";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;
    RememberLocation(a.def.id, file);
    jobs::Scan({a.def});
    Toast(a.def.name + L" found.");
}

static void OpenFolder(AppState& a) {
    if (a.inst.dir.empty()) return;
    ShellExecuteW(g_hwnd, L"open", a.inst.dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void OpenUrl(const std::wstring& url) { ShellExecuteW(g_hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// ---------------------------------------------------------------- refresh
static void ScanAll() {
    g_lastScan = GetTickCount();
    InvalidateDiscoveryCache();
    jobs::Scan(g_manifest.apps);
}

static void PollProcesses() {
    std::vector<std::wstring> ids, exes, dirs;
    for (auto& a : g_apps) {
        ids.push_back(a.def.id);
        exes.push_back(a.inst.installed ? a.inst.exe : L"");
        // Electron/Tauri apps run helpers from their own folder; single-exe apps share a folder
        // with nothing else, but match on the exact path for them to be safe
        dirs.push_back(a.inst.installed && a.def.type != InstallType::Portable ? a.inst.dir : L"");
    }
    jobs::ScanProcesses(ids, exes, dirs);
}

static void CheckReleases(bool force) {
    for (auto& a : g_apps) {
        a.checking = true;
        jobs::FetchReleases(a.def, force);
    }
    Invalidate();
}

static UINT IconPx() { return (UINT)std::min(160.0f, std::ceil(76 * g_scale)); }

static void ApplyManifest(const std::wstring& text, bool fromNetwork) {
    Manifest m;
    std::wstring err;
    if (!ParseManifest(text, m, &err)) { g_manifestError = err; return; }
    // a local override lists apps that aren't public yet, or tweaks entries while testing
    std::wstring localText;
    if (ReadTextFile(DataDir() + L"\\manifest.local.json", localText)) {
        Manifest local;
        std::wstring lerr;
        if (ParseManifest(localText, local, &lerr, true)) MergeManifest(m, local);
        else Log(L"manifest.local.json: %ls", lerr.c_str());
    }
    g_manifest = m;
    g_manifestText = text;
    g_manifestError.clear();
    // rebuild the app list, keeping what we already know about each app
    std::vector<AppState> next;
    for (auto& d : m.apps) {
        AppState s;
        if (AppState* old = FindApp(d.id)) {
            s = *old;
            old->icon = nullptr;   // ownership moves
            bool iconChanged = old->def.icon != d.icon;
            s.def = d;
            if (iconChanged) { SafeRelease(s.icon); s.iconPixels.clear(); jobs::FetchIcon(d, IconPx()); }
        } else {
            s.def = d;
            jobs::LoadCachedReleases(d);
            jobs::FetchIcon(d, IconPx());
            if (fromNetwork) { s.checking = true; jobs::FetchReleases(d, false); }
        }
        next.push_back(std::move(s));
    }
    for (auto& a : g_apps) SafeRelease(a.icon);
    g_apps = std::move(next);
    if (g_detail >= (int)g_apps.size()) { g_detail = -1; g_page = Page::Library; }
    ScanAll();
}

// ---------------------------------------------------------------- render target
static void DiscardDevice() {
    for (auto& a : g_apps) SafeRelease(a.icon);
    SafeRelease(g_br);
    SafeRelease(g_rt);
}

static bool EnsureDevice() {
    if (g_rt) return true;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    UINT dpi = GetDpiForWindow(g_hwnd);
    props.dpiX = props.dpiY = (float)dpi;
    if (FAILED(d2d->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom)),
                                           D2D1_PRESENT_OPTIONS_NONE), &g_rt))) return false;
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &g_br);
    return true;
}

static ID2D1Bitmap* IconBitmap(AppState& a) {
    if (a.icon || a.iconPixels.empty() || !g_rt) return a.icon;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    g_rt->CreateBitmap(D2D1::SizeU(a.iconW, a.iconH), a.iconPixels.data(), a.iconW * 4, bp, &a.icon);
    return a.icon;
}

// ---------------------------------------------------------------- drawing helpers
static Canvas g_c;

static void AddHot(const D2D1_RECT_F& r, Hit kind, int index = -1, int arg = 0, bool enabled = true) {
    g_hot.push_back({r, kind, index, arg, enabled});
}
static bool IsHover(Hit kind, int index = -1, int arg = 0) {
    return g_hover.kind == kind && g_hover.index == index && g_hover.arg == arg;
}
static bool IsPressed(Hit kind, int index = -1, int arg = 0) {
    return g_pressed.kind == kind && g_pressed.index == index && g_pressed.arg == arg && IsHover(kind, index, arg);
}

enum class Btn { Primary, Secondary, Subtle, Danger };

static float ButtonWidth(const std::wstring& label, wchar_t glyph) {
    float w = MeasureWidth(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 28;
    if (glyph) w += label.empty() ? 6 : 22;
    return std::max(w, label.empty() ? 34.0f : 76.0f);
}

static void Button(const D2D1_RECT_F& r, const std::wstring& label, wchar_t glyph, Btn style, Hit kind, int index = -1, int arg = 0, bool enabled = true) {
    bool hot = enabled && IsHover(kind, index, arg);
    bool down = enabled && IsPressed(kind, index, arg);
    D2D1_COLOR_F bg, fg;
    switch (style) {
        case Btn::Primary: bg = hot ? g_t.accentHover : g_t.accent; fg = g_t.onAccent; break;
        case Btn::Danger: bg = hot ? Alpha(g_t.bad, 0.22f) : Alpha(g_t.bad, 0.12f); fg = g_t.bad; break;
        case Btn::Subtle: bg = hot ? g_t.surfaceHover : Alpha(g_t.surface, 0.0f); fg = g_t.text; break;
        default: bg = hot ? g_t.surfaceStrong : Mix(g_t.surface, g_t.surfaceStrong, 0.55f); fg = g_t.text; break;
    }
    if (!enabled) { bg = Alpha(g_t.surfaceStrong, 0.5f); fg = g_t.faint; }
    if (down) bg = Mix(bg, g_t.dark ? Rgb(0x000000) : Rgb(0xFFFFFF), 0.12f);
    g_c.FillRound(r, 6, bg);
    if (style == Btn::Secondary && enabled) g_c.StrokeRound(r, 6, g_t.divider);
    float cx = r.left, w = r.right - r.left;
    auto* tf = Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (glyph && label.empty()) {
        g_c.Glyph(glyph, Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), r, fg);
    } else if (glyph) {
        float tw = MeasureWidth(label, tf) + 22;
        float x = cx + (w - tw) / 2;
        g_c.Glyph(glyph, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, false, true), D2D1::RectF(x, r.top, x + 18, r.bottom), fg);
        g_c.Text(label, Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x + 22, r.top, r.right, r.bottom), fg);
    } else {
        g_c.Text(label, tf, r, fg);
    }
    AddHot(r, kind, index, arg, enabled);
}

static float Pill(float x, float y, const std::wstring& text, D2D1_COLOR_F color, bool rightAlign = false) {
    if (text.empty()) return 0;
    auto* f = Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    float w = MeasureWidth(text, f) + 16;
    if (rightAlign) x -= w;
    D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + 20);
    g_c.FillRound(r, 10, Alpha(color, g_t.dark ? 0.16f : 0.12f));
    g_c.Text(text, f, r, color);
    return w;
}

static void ProgressBar(const D2D1_RECT_F& r, float pct, D2D1_COLOR_F color) {
    g_c.FillRound(r, (r.bottom - r.top) / 2, g_t.surfaceStrong);
    float w = r.right - r.left;
    if (pct < 0) {
        // indeterminate: a segment sliding back and forth
        float t = (GetTickCount() % 1400) / 1400.0f;
        float seg = w * 0.3f;
        float x = r.left + (w + seg) * t - seg;
        D2D1_RECT_F s = D2D1::RectF(std::max(r.left, x), r.top, std::min(r.right, x + seg), r.bottom);
        if (s.right > s.left) g_c.FillRound(s, (r.bottom - r.top) / 2, color);
    } else {
        D2D1_RECT_F s = r;
        s.right = r.left + w * std::min(1.0f, pct / 100.0f);
        if (s.right - s.left > 1) g_c.FillRound(s, (r.bottom - r.top) / 2, color);
    }
}

static void DrawAppIcon(AppState& a, const D2D1_RECT_F& r) {
    if (ID2D1Bitmap* bmp = IconBitmap(a)) {
        D2D1_SIZE_F s = bmp->GetSize();
        float w = r.right - r.left, h = r.bottom - r.top;
        float k = std::min(w / s.width, h / s.height);
        float dw = s.width * k, dh = s.height * k;
        D2D1_RECT_F d = D2D1::RectF(r.left + (w - dw) / 2, r.top + (h - dh) / 2, r.left + (w + dw) / 2, r.top + (h + dh) / 2);
        g_rt->DrawBitmap(bmp, d, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    // placeholder: a soft tile with the app's initial, colored from its name
    uint32_t h = 0;
    for (wchar_t c : a.def.name) h = h * 31 + c;
    static const uint32_t palette[] = {0x5B8DEF, 0x9B6CF2, 0xE86A92, 0xF29B4B, 0x3FB98E, 0x47B4D6, 0xD9634C, 0x7A86A8};
    D2D1_COLOR_F col = Rgb(palette[h % 8]);
    float rad = (r.right - r.left) * 0.24f;
    g_c.FillRound(r, rad, Alpha(col, 0.22f));
    std::wstring init = a.def.name.substr(0, 1);
    g_c.Text(init, Format((r.bottom - r.top) * 0.46f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER), r, col);
}

// Turns GitHub release notes (Markdown) into readable plain text.
static std::wstring CleanNotes(const std::wstring& md) {
    std::wstring out;
    size_t i = 0;
    int blank = 0;
    while (i <= md.size()) {
        size_t e = md.find(L'\n', i);
        if (e == std::wstring::npos) e = md.size();
        std::wstring line = md.substr(i, e - i);
        i = e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring t = Trim(line);
        if (t.empty()) { if (++blank <= 1 && !out.empty()) out += L"\n"; continue; }
        blank = 0;
        size_t indent = line.find_first_not_of(L" \t");
        while (!t.empty() && t[0] == L'#') t.erase(0, 1);
        t = Trim(t);
        if (t.size() > 1 && (t[0] == L'-' || t[0] == L'*' || t[0] == L'+') && t[1] == L' ') t = std::wstring(indent >= 2 ? L"    \x25E6 " : L"\x2022  ") + Trim(t.substr(2));
        if (t == L"---" || t == L"***") continue;
        // [text](url) -> text, drop images
        std::wstring s;
        for (size_t k = 0; k < t.size(); ++k) {
            if (t[k] == L'!' && k + 1 < t.size() && t[k + 1] == L'[') continue;
            if (t[k] == L'[') {
                size_t close = t.find(L"](", k);
                size_t paren = close == std::wstring::npos ? std::wstring::npos : t.find(L')', close);
                if (close != std::wstring::npos && paren != std::wstring::npos) { s += t.substr(k + 1, close - k - 1); k = paren; continue; }
            }
            if (t[k] == L'*' || t[k] == L'`' || (t[k] == L'_' && k + 1 < t.size() && t[k + 1] == L'_')) { if (t[k] == L'_') ++k; continue; }
            s += t[k];
        }
        out += s + L"\n";
    }
    while (!out.empty() && out.back() == L'\n') out.pop_back();
    if (out.size() > 4000) out = out.substr(0, 4000) + L"…";
    return out;
}

static std::wstring AgoFromUnix(long long t) {
    long long ago = (long long)time(nullptr) - t;
    if (ago < 90) return L"Just now";
    if (ago < 3600) return std::to_wstring(ago / 60) + L" min ago";
    if (ago < 86400) return std::to_wstring(ago / 3600) + L" h ago";
    return std::to_wstring(ago / 86400) + L" days ago";
}

static std::wstring ShortDate(const std::wstring& iso) {
    int Y, M, D;
    if (swscanf(iso.c_str(), L"%d-%d-%d", &Y, &M, &D) != 3) return L"";
    static const wchar_t* mon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    wchar_t b[32];
    swprintf(b, 32, L"%ls %d, %d", mon[(M + 11) % 12], D, Y);
    return b;
}

// ---------------------------------------------------------------- sidebar
static const float kSidebar = 244;
static const float kHeader = 76;

static wchar_t CategoryGlyph(const std::wstring& id) {
    if (id == L"windows") return icon::Windows;
    if (id == L"audio") return icon::Audio;
    if (id == L"gaming") return icon::Game;
    if (id == L"media") return icon::Media;
    if (id == L"work") return icon::Work;
    return icon::Apps;
}

static bool MatchesNav(const AppState& a, const std::wstring& nav) {
    if (nav == L"all") return true;
    if (nav == L"installed") return a.inst.installed;
    if (nav == L"updates") return HasUpdate(a) || (a.busy && a.op == OpKind::Update);
    return a.def.category == nav;
}

static void NavItem(float y, const std::wstring& id, const std::wstring& label, wchar_t glyph, int count, bool badge, int arg) {
    D2D1_RECT_F r = D2D1::RectF(10, y, kSidebar - 10, y + 36);
    bool sel = (g_page == Page::Library && g_nav == id) || (id == L"settings" && g_page == Page::Settings);
    bool hot = IsHover(Hit::Nav, -1, arg);
    if (sel) g_c.FillRound(r, 6, g_t.dark ? Rgb(0xFFFFFF, 0.08f) : Rgb(0x000000, 0.06f));
    else if (hot) g_c.FillRound(r, 6, g_t.dark ? Rgb(0xFFFFFF, 0.04f) : Rgb(0x000000, 0.035f));
    if (sel) g_c.FillRound(D2D1::RectF(r.left, y + 10, r.left + 3, y + 26), 1.5f, g_t.accent);
    g_c.Glyph(glyph, Format(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 10, y, r.left + 34, y + 36),
              sel ? g_t.accent : g_t.subtext);
    g_c.Text(label, Format(13.5f, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL), D2D1::RectF(r.left + 42, y, r.right - 40, y + 36), g_t.text);
    if (count > 0 || (count == 0 && !badge && id != L"settings")) {
        std::wstring n = std::to_wstring(count);
        if (badge && count > 0) {
            auto* f = Format(11.5f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
            float w = std::max(22.0f, MeasureWidth(n, f) + 12);
            D2D1_RECT_F b = D2D1::RectF(r.right - 10 - w, y + 8, r.right - 10, y + 28);
            g_c.FillRound(b, 10, g_t.accent);
            g_c.Text(n, f, b, g_t.onAccent);
        } else {
            g_c.Text(n, Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(r.left, y, r.right - 12, y + 36), g_t.faint);
        }
    }
    AddHot(r, Hit::Nav, -1, arg);
}

static std::vector<std::wstring> g_navIds;   // arg -> nav id

static std::wstring StatusLine() {
    if (long long until = jobs::RateLimitedUntil()) {
        long long mins = std::max(1LL, (until - (long long)time(nullptr) + 59) / 60);
        return L"GitHub limit reached · retry in " + std::to_wstring(mins) + L" min";
    }
    if (g_offline) return L"Offline · showing saved info";
    bool checking = false;
    long long last = 0;
    for (auto& a : g_apps) { checking |= a.checking; last = std::max(last, a.checkedAt); }
    if (checking) return L"Checking for updates…";
    if (!last) return L"";
    long long ago = (long long)time(nullptr) - last;
    if (ago < 90) return L"Checked just now";
    if (ago < 3600) return L"Checked " + std::to_wstring(ago / 60) + L" min ago";
    if (ago < 86400) return L"Checked " + std::to_wstring(ago / 3600) + L" h ago";
    return L"Checked " + std::to_wstring(ago / 86400) + L" d ago";
}

static void DrawSidebar() {
    g_c.Fill(D2D1::RectF(0, 0, kSidebar, g_H), g_t.sidebar);
    g_c.Line(kSidebar - 0.5f, 0, kSidebar - 0.5f, g_H, g_t.divider);
    // brand
    D2D1_RECT_F logo = D2D1::RectF(22, 22, 50, 50);
    g_c.FillRound(logo, 8, g_t.accent);
    // a little hangar roofline
    ID2D1PathGeometry* geo = nullptr;
    if (SUCCEEDED(d2d->CreatePathGeometry(&geo))) {
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(geo->Open(&s))) {
            s->BeginFigure(D2D1::Point2F(28, 43), D2D1_FIGURE_BEGIN_FILLED);
            s->AddLine(D2D1::Point2F(28, 35));
            s->AddBezier(D2D1::BezierSegment(D2D1::Point2F(28, 27.5f), D2D1::Point2F(44, 27.5f), D2D1::Point2F(44, 35)));
            s->AddLine(D2D1::Point2F(44, 43));
            s->AddLine(D2D1::Point2F(40, 43));
            s->AddLine(D2D1::Point2F(40, 37));
            s->AddLine(D2D1::Point2F(32, 37));
            s->AddLine(D2D1::Point2F(32, 43));
            s->EndFigure(D2D1_FIGURE_END_CLOSED);
            s->Close();
            s->Release();
            g_br->SetColor(g_t.onAccent);
            g_rt->FillGeometry(geo, g_br);
        }
        geo->Release();
    }
    g_c.Text(L"Hangar", Format(17, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(60, 20, kSidebar, 40), g_t.text);
    g_c.Text(L"Your app suite", Format(11.5f), D2D1::RectF(60, 38, kSidebar, 54), g_t.subtext);

    g_navIds.clear();
    float y = 76;
    auto item = [&](const std::wstring& id, const std::wstring& label, wchar_t glyph, int count, bool badge) {
        g_navIds.push_back(id);
        NavItem(y, id, label, glyph, count, badge, (int)g_navIds.size() - 1);
        y += 38;
    };
    int installed = 0;
    for (auto& a : g_apps) if (a.inst.installed) ++installed;
    item(L"all", L"All apps", icon::Library, (int)g_apps.size(), false);
    item(L"installed", L"Installed", icon::Installed, installed, false);
    item(L"updates", L"Updates", icon::Update, UpdateCount(), true);
    y += 10;
    g_c.Text(L"CATEGORIES", Format(11, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(24, y, kSidebar, y + 22), g_t.faint);
    y += 26;
    std::vector<std::wstring> cats;
    for (auto& c : g_manifest.categories) cats.push_back(c.id);
    for (auto& a : g_apps) if (std::find(cats.begin(), cats.end(), a.def.category) == cats.end()) cats.push_back(a.def.category);
    for (auto& c : cats) {
        int n = 0;
        for (auto& a : g_apps) if (a.def.category == c) ++n;
        if (n) item(c, CategoryName(c), CategoryGlyph(c), n, false);
    }
    // bottom
    float by = g_H - 82;
    if (g_self.kind == SelfUpdateState::Ready && y + 100 < by) {
        D2D1_RECT_F b = D2D1::RectF(12, by - 92, kSidebar - 12, by - 8);
        g_c.FillRound(b, 8, Alpha(g_t.accent, 0.14f));
        g_c.StrokeRound(b, 8, Alpha(g_t.accent, 0.35f));
        g_c.Text(L"Hangar " + g_self.version + L" is ready", Format(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(b.left + 12, b.top + 8, b.right - 8, b.top + 28), g_t.text);
        g_c.Text(jobs::ActiveOps() ? L"Installs after the current updates" : L"Takes a second · you stay where you are", Format(11.5f), D2D1::RectF(b.left + 12, b.top + 27, b.right - 8, b.top + 44), g_t.subtext);
        Button(D2D1::RectF(b.left + 10, b.bottom - 34, b.right - 10, b.bottom - 8), L"Restart to update", icon::Update, Btn::Primary, Hit::SelfRestart);
    }
    if (y + 10 < by) {
        g_navIds.push_back(L"settings");
        NavItem(by, L"settings", L"Settings", icon::Settings, -1, false, (int)g_navIds.size() - 1);
    }
    g_c.Text(StatusLine(), Format(11.5f), D2D1::RectF(24, g_H - 40, kSidebar - 12, g_H - 18), g_t.faint);
}

// ---------------------------------------------------------------- library
static void ComputeVisible() {
    g_visible.clear();
    for (size_t i = 0; i < g_apps.size(); ++i) {
        const AppState& a = g_apps[i];
        if (!MatchesNav(a, g_nav)) continue;
        if (!g_search.empty() && !IContains(a.def.name, g_search) && !IContains(a.def.description, g_search) &&
            !IContains(CategoryName(a.def.category), g_search)) continue;
        g_visible.push_back((int)i);
    }
}

static std::wstring PageTitle() {
    if (g_nav == L"all") return L"All apps";
    if (g_nav == L"installed") return L"Installed";
    if (g_nav == L"updates") return L"Updates";
    return CategoryName(g_nav);
}

static void DrawCardAction(AppState& a, int i, const D2D1_RECT_F& r) {
    std::wstring label;
    bool enabled;
    Action act = PrimaryAction(a, &label, &enabled);
    Btn style = act == Action::Install || act == Action::Update ? Btn::Primary : Btn::Secondary;
    wchar_t glyph = act == Action::Install ? icon::Download : act == Action::Update ? icon::Update : act == Action::Open ? icon::Play : 0;
    Button(r, label, glyph, style, Hit::CardAction, i, 0, enabled);
}

static void DrawCard(int i, const D2D1_RECT_F& r, bool focused) {
    AppState& a = g_apps[i];
    D2D1_COLOR_F bg = Mix(g_t.surface, g_t.surfaceHover, a.hover);
    g_c.FillRound(r, 10, bg);
    g_c.StrokeRound(r, 10, focused && g_keyboardMode ? g_t.accent : Mix(g_t.divider, Alpha(g_t.accent, 0.5f), a.hover * 0.4f), focused && g_keyboardMode ? 2.0f : 1.0f);
    AddHot(r, Hit::Card, i);
    float x = r.left + 16, y = r.top + 16;
    DrawAppIcon(a, D2D1::RectF(x, y, x + 44, y + 44));
    StatusInfo st = Status(a);
    if (!a.inst.installed && !a.busy && a.lastError.empty()) st.label.clear();   // the Install button says it
    float pillW = st.label.empty() ? 0 : MeasureWidth(st.label, Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER)) + 16;
    g_c.Text(a.def.name, Format(15.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x + 56, y - 1, r.right - pillW - 22, y + 22), g_t.text);
    g_c.Text(VersionLine(a), Format(12.5f), D2D1::RectF(x + 56, y + 22, r.right - 16, y + 42), HasUpdate(a) ? g_t.warn : g_t.subtext);
    if (!st.label.empty()) Pill(r.right - 14, y + 1, st.label, st.color, true);
    D2D1_RECT_F desc = D2D1::RectF(x, y + 56, r.right - 16, y + 92);
    g_c.Text(a.def.description, Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), desc, g_t.subtext);

    float by = r.bottom - 44;
    std::wstring label;
    bool en;
    PrimaryAction(a, &label, &en);
    float bw = ButtonWidth(label, a.busy ? 0 : icon::Play);
    D2D1_RECT_F btn = D2D1::RectF(r.right - 14 - bw, by, r.right - 14, by + 30);
    if (a.busy) {
        float pct = a.percent < 0 ? -1 : a.shownPercent;
        ProgressBar(D2D1::RectF(x, by + 22, btn.left - 14, by + 26), pct, g_t.accent);
        std::wstring t = a.opText;
        g_c.Text(t, Format(12), D2D1::RectF(x, by, btn.left - 14, by + 18), g_t.subtext);
    } else if (!a.lastError.empty()) {
        g_c.Text(a.lastError, Format(12), D2D1::RectF(x, by, btn.left - 12, by + 30), g_t.bad);
    } else {
        std::wstring left = CategoryName(a.def.category);
        g_c.Text(left, Format(12), D2D1::RectF(x, by, btn.left - 12, by + 30), g_t.faint);
    }
    DrawCardAction(a, i, btn);
}

static void DrawRow(int i, const D2D1_RECT_F& r, bool focused) {
    AppState& a = g_apps[i];
    D2D1_COLOR_F bg = Mix(g_t.surface, g_t.surfaceHover, a.hover);
    g_c.FillRound(r, 8, bg);
    if (focused && g_keyboardMode) g_c.StrokeRound(r, 8, g_t.accent, 2);
    AddHot(r, Hit::Card, i);
    float h = r.bottom - r.top;
    float x = r.left + 14, cy = r.top + h / 2;
    DrawAppIcon(a, D2D1::RectF(x, cy - 18, x + 36, cy + 18));
    float w = r.right - r.left;
    float colStatus = r.right - 150 - 130, colVer = colStatus - 170;
    bool narrow = w < 700;
    float textRight = narrow ? colStatus - 10 : colVer - 16;
    g_c.Text(a.def.name, Format(14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(x + 50, r.top + 10, textRight, r.top + 32), g_t.text);
    g_c.Text(a.def.description, Format(12.5f), D2D1::RectF(x + 50, r.top + 32, textRight, r.top + 52), g_t.subtext);
    if (!narrow) g_c.Text(VersionLine(a), Format(12.5f), D2D1::RectF(colVer, r.top, colStatus - 10, r.bottom), HasUpdate(a) ? g_t.warn : g_t.subtext);
    std::wstring label;
    bool en;
    PrimaryAction(a, &label, &en);
    float bw = ButtonWidth(label, a.busy ? 0 : icon::Play);
    D2D1_RECT_F btn = D2D1::RectF(r.right - 14 - bw, cy - 15, r.right - 14, cy + 15);
    if (a.busy) {
        float pct = a.percent < 0 ? -1 : a.shownPercent;
        g_c.Text(a.opText, Format(12), D2D1::RectF(colStatus, r.top + 10, btn.left - 12, r.top + 32), g_t.subtext);
        ProgressBar(D2D1::RectF(colStatus, r.top + 38, btn.left - 14, r.top + 42), pct, g_t.accent);
    } else {
        StatusInfo st = Status(a);
        Pill(colStatus, cy - 10, st.label, st.color);
    }
    DrawCardAction(a, i, btn);
}

static void DrawSearch(const D2D1_RECT_F& r) {
    bool hot = IsHover(Hit::Search);
    g_c.FillRound(r, 6, g_searchFocus ? g_t.surface : (hot ? g_t.surfaceHover : Mix(g_t.surface, g_t.bg, 0.3f)));
    g_c.StrokeRound(r, 6, g_searchFocus ? g_t.accent : g_t.divider, g_searchFocus ? 1.5f : 1.0f);
    g_c.Glyph(icon::Search, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 6, r.top, r.left + 32, r.bottom), g_t.subtext);
    auto* f = Format(13);
    D2D1_RECT_F tr = D2D1::RectF(r.left + 34, r.top, r.right - 30, r.bottom);
    if (g_search.empty()) g_c.Text(g_searchFocus ? L"" : L"Search apps   Ctrl+K", f, tr, g_t.faint);
    else g_c.Text(g_search, f, tr, g_t.text);
    if (g_searchFocus && g_caretOn) {
        float cx = tr.left + std::min(MeasureWidth(g_search, f), tr.right - tr.left - 2);
        g_c.Fill(D2D1::RectF(cx, r.top + 8, cx + 1.2f, r.bottom - 8), g_t.text);
    }
    AddHot(r, Hit::Search);
    if (!g_search.empty()) {
        D2D1_RECT_F cr = D2D1::RectF(r.right - 28, r.top + 4, r.right - 4, r.bottom - 4);
        if (IsHover(Hit::SearchClear)) g_c.FillRound(cr, 4, g_t.surfaceStrong);
        g_c.Glyph(icon::Cancel, Format(10, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), cr, g_t.subtext);
        AddHot(cr, Hit::SearchClear);
    }
}

static void DrawLibrary(float x0) {
    ComputeVisible();
    float pad = 28;
    float left = x0 + pad, right = g_W - pad;
    float top = kHeader;
    g_viewH = g_H - top;
    // content
    g_rt->PushAxisAlignedClip(D2D1::RectF(x0, top, g_W, g_H), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = top + 6 - g_scroll;
    if (g_visible.empty()) {
        std::wstring msg = !g_search.empty() ? L"No apps match \"" + g_search + L"\"."
                         : g_nav == L"updates" ? L"Everything is up to date."
                         : g_nav == L"installed" ? L"Nothing installed yet. Pick an app from All apps to get started."
                         : g_apps.empty() ? (g_manifestError.empty() ? L"Loading the app list…" : g_manifestError) : L"No apps here yet.";
        g_c.Glyph(g_nav == L"updates" && g_search.empty() ? icon::Check : icon::Apps, Format(34, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true),
                  D2D1::RectF(left, y + 70, right, y + 120), g_t.faint);
        g_c.Text(msg, Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), D2D1::RectF(left, y + 124, right, y + 150), g_t.subtext);
        g_contentH = 200;
    } else if (!g_cfg.listView) {
        float gap = 14, minW = 280;
        float avail = right - left;
        g_cols = std::max(1, (int)((avail + gap) / (minW + gap)));
        float cw = (avail - gap * (g_cols - 1)) / g_cols, ch = 150;
        for (size_t k = 0; k < g_visible.size(); ++k) {
            int col = (int)(k % g_cols), row = (int)(k / g_cols);
            D2D1_RECT_F r = D2D1::RectF(left + col * (cw + gap), y + row * (ch + gap), left + col * (cw + gap) + cw, y + row * (ch + gap) + ch);
            if (r.bottom >= top && r.top <= g_H) DrawCard(g_visible[k], r, (int)k == g_focus);
        }
        int rows = ((int)g_visible.size() + g_cols - 1) / g_cols;
        g_contentH = rows * (ch + gap) + 20;
    } else {
        g_cols = 1;
        float rh = 64, gap = 6;
        for (size_t k = 0; k < g_visible.size(); ++k) {
            D2D1_RECT_F r = D2D1::RectF(left, y + k * (rh + gap), right, y + k * (rh + gap) + rh);
            if (r.bottom >= top && r.top <= g_H) DrawRow(g_visible[k], r, (int)k == g_focus);
        }
        g_contentH = g_visible.size() * (rh + gap) + 20;
    }
    g_rt->PopAxisAlignedClip();

    // header (drawn after the content so it stays on top)
    g_c.Fill(D2D1::RectF(x0, 0, g_W, top), g_t.bg);
    if (g_scroll > 1) g_c.Line(x0, top - 0.5f, g_W, top - 0.5f, g_t.divider);
    g_c.Text(PageTitle(), Format(22, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, 16, right, 46), g_t.text);
    std::wstring sub = std::to_wstring(g_visible.size()) + (g_visible.size() == 1 ? L" app" : L" apps");
    g_c.Text(sub, Format(12.5f), D2D1::RectF(left, 44, right, 62), g_t.subtext);

    float bx = right;
    // view toggle
    D2D1_RECT_F vl = D2D1::RectF(bx - 34, 22, bx, 54);
    D2D1_RECT_F vg = D2D1::RectF(bx - 68, 22, bx - 34, 54);
    g_c.FillRound(D2D1::RectF(vg.left, vg.top, vl.right, vl.bottom), 6, Mix(g_t.surface, g_t.bg, 0.3f));
    g_c.StrokeRound(D2D1::RectF(vg.left, vg.top, vl.right, vl.bottom), 6, g_t.divider);
    auto seg = [&](const D2D1_RECT_F& r, wchar_t g, bool sel, Hit h) {
        D2D1_RECT_F in = D2D1::RectF(r.left + 3, r.top + 3, r.right - 3, r.bottom - 3);
        if (sel) g_c.FillRound(in, 4, g_t.surfaceStrong);
        else if (IsHover(h)) g_c.FillRound(in, 4, g_t.surfaceHover);
        g_c.Glyph(g, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), r, sel ? g_t.text : g_t.subtext);
        AddHot(r, h);
    };
    seg(vg, icon::Grid, !g_cfg.listView, Hit::ViewGrid);
    seg(vl, icon::List, g_cfg.listView, Hit::ViewList);
    bx -= 78;
    bool anyChecking = false;
    for (auto& a : g_apps) anyChecking |= a.checking;
    Button(D2D1::RectF(bx - 34, 22, bx, 54), L"", icon::Refresh, Btn::Secondary, Hit::Refresh, -1, 0, !anyChecking);
    bx -= 44;
    int n = UpdateCount();
    if (n > 0) {
        std::wstring label = L"Update all (" + std::to_wstring(n) + L")";
        float w = ButtonWidth(label, icon::Update);
        Button(D2D1::RectF(bx - w, 22, bx, 54), label, icon::Update, Btn::Primary, Hit::UpdateAll);
        bx -= w + 10;
    }
    float sw = std::min(280.0f, bx - left - 170);
    if (sw > 120) DrawSearch(D2D1::RectF(bx - sw, 22, bx, 54));
}

// ---------------------------------------------------------------- details
static void DrawDetails(float x0) {
    if (g_detail < 0 || g_detail >= (int)g_apps.size()) { g_page = Page::Library; return; }
    AppState& a = g_apps[g_detail];
    float pad = 32;
    float left = x0 + pad, right = std::min(g_W - pad, x0 + pad + 980);
    float top = 0;
    g_viewH = g_H;
    g_rt->PushAxisAlignedClip(D2D1::RectF(x0, top, g_W, g_H), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = 18 - g_scroll;

    Button(D2D1::RectF(left - 8, y, left + 92, y + 30), L"Library", icon::Back, Btn::Subtle, Hit::Back);
    y += 48;
    DrawAppIcon(a, D2D1::RectF(left, y, left + 76, y + 76));
    float tx = left + 96;
    g_c.Text(a.def.name, Format(28, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(tx, y - 2, right, y + 36), g_t.text);
    std::wstring meta = CategoryName(a.def.category) + L"  ·  " + InstallTypeName(a.def.type);
    if (a.def.critical) meta += L"  ·  Core app";
    if (a.def.local) meta += L"  ·  Local manifest";
    g_c.Text(meta, Format(13), D2D1::RectF(tx, y + 36, right, y + 56), g_t.subtext);
    StatusInfo st = Status(a);
    float px = tx;
    px += Pill(px, y + 60, st.label, st.color) + 8;
    if (Running(a) && st.label != L"Running") px += Pill(px, y + 60, L"Running", g_t.good) + 8;
    if (UsesBeta(a)) Pill(px, y + 60, L"Beta channel", g_t.warn);
    y += 96;
    g_c.Text(a.def.description, Format(14.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true), D2D1::RectF(left, y, right, y + 44), g_t.text);
    y += 48;

    // actions
    float bx = left;
    std::wstring label;
    bool en;
    Action act = PrimaryAction(a, &label, &en);
    if (act == Action::Cancel) {
        float w = ButtonWidth(L"Cancel", icon::Cancel);
        Button(D2D1::RectF(bx, y, bx + w, y + 34), L"Cancel", icon::Cancel, Btn::Secondary, Hit::DCancel, g_detail, 0, en);
        bx += w + 10;
    } else {
        wchar_t g = act == Action::Install ? icon::Download : act == Action::Update ? icon::Update : act == Action::Open ? icon::Play : 0;
        std::wstring l = act == Action::Install && Latest(a) ? L"Install " + Latest(a)->version : act == Action::Update ? L"Update to " + Latest(a)->version : label;
        float w = ButtonWidth(l, g) + 8;
        Button(D2D1::RectF(bx, y, bx + w, y + 34), l, g, act == Action::Open ? Btn::Secondary : Btn::Primary, Hit::DAction, g_detail, 0, en);
        bx += w + 10;
        if (act == Action::Update) {
            float w2 = ButtonWidth(L"Open", icon::Play);
            Button(D2D1::RectF(bx, y, bx + w2, y + 34), L"Open", icon::Play, Btn::Secondary, Hit::DAction, g_detail, 1);
            bx += w2 + 10;
        }
    }
    if (a.inst.installed) {
        float w = ButtonWidth(L"Open folder", icon::Folder);
        Button(D2D1::RectF(bx, y, bx + w, y + 34), L"Open folder", icon::Folder, Btn::Secondary, Hit::DOpenFolder, g_detail, 0, !a.busy);
        bx += w + 10;
        w = ButtonWidth(L"Repair", icon::Repair);
        Button(D2D1::RectF(bx, y, bx + w, y + 34), L"Repair", icon::Repair, Btn::Secondary, Hit::DRepair, g_detail, 0, !a.busy && !a.releases.empty());
        bx += w + 10;
        w = ButtonWidth(L"Uninstall", icon::Delete);
        Button(D2D1::RectF(bx, y, bx + w, y + 34), L"Uninstall", icon::Delete, Btn::Danger, Hit::DUninstall, g_detail, 0, !a.busy);
        bx += w + 10;
    } else if (!a.busy) {
        float w = ButtonWidth(L"Already installed? Locate it", icon::Search);
        Button(D2D1::RectF(bx, y, bx + w, y + 34), L"Already installed? Locate it", icon::Search, Btn::Subtle, Hit::DLocate, g_detail);
        bx += w + 10;
    }
    y += 46;
    if (a.busy) {
        g_c.Text(a.opText, Format(13), D2D1::RectF(left, y, right, y + 20), g_t.subtext);
        ProgressBar(D2D1::RectF(left, y + 26, std::min(right, left + 520), y + 31), a.percent < 0 ? -1 : a.shownPercent, g_t.accent);
        y += 46;
    } else if (!a.lastError.empty()) {
        D2D1_RECT_F er = D2D1::RectF(left, y, right, y + 40);
        g_c.FillRound(er, 8, Alpha(g_t.bad, 0.12f));
        g_c.Glyph(icon::Warning, Format(14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(left + 6, y, left + 36, y + 40), g_t.bad);
        g_c.Text(a.lastError, Format(13), D2D1::RectF(left + 40, y, right - 12, y + 40), g_t.text);
        y += 54;
    }

    // facts
    const ReleaseInfo* stable = LatestRelease(a.releases, a.def, false);
    const ReleaseInfo* beta = LatestRelease(a.releases, a.def, true);
    struct Fact { std::wstring k, v; };
    std::vector<Fact> facts = {
        {L"Installed", a.inst.installed ? (a.inst.version.empty() ? L"Yes (version unknown)" : a.inst.version) : L"No"},
        {L"Latest stable", stable ? stable->version : (a.releasesLoaded ? L"None" : L"…")},
        {L"Latest beta", beta && beta->prerelease ? beta->version : L"None newer"},
        {L"Last checked", a.checkedAt ? AgoFromUnix(a.checkedAt) : L"Never"},
    };
    float fw = (right - left - 12 * 3) / 4;
    for (size_t k = 0; k < facts.size(); ++k) {
        D2D1_RECT_F r = D2D1::RectF(left + k * (fw + 12), y, left + k * (fw + 12) + fw, y + 64);
        g_c.FillRound(r, 8, g_t.surface);
        g_c.Text(facts[k].k, Format(12), D2D1::RectF(r.left + 14, r.top + 10, r.right - 10, r.top + 28), g_t.subtext);
        g_c.Text(facts[k].v, Format(16, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 14, r.top + 28, r.right - 10, r.top + 54), g_t.text);
    }
    y += 80;

    // channel + location
    g_c.Text(L"Release channel", Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, left + 200, y + 32), g_t.text);
    float sx = left + 140;
    for (int k = 0; k < 2; ++k) {
        bool sel = (k == 1) == UsesBeta(a);
        D2D1_RECT_F r = D2D1::RectF(sx + k * 84, y, sx + k * 84 + 80, y + 32);
        g_c.FillRound(r, 6, sel ? Alpha(g_t.accent, 0.18f) : (IsHover(Hit::DChannel, g_detail, k) ? g_t.surfaceHover : g_t.surface));
        if (sel) g_c.StrokeRound(r, 6, Alpha(g_t.accent, 0.6f));
        g_c.Text(k ? L"Beta" : L"Stable", Format(13, sel ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), r, sel ? g_t.text : g_t.subtext);
        AddHot(r, Hit::DChannel, g_detail, k);
    }
    float lx = sx + 190;
    std::wstring repoLabel = L"github.com/" + a.def.repo;
    float rw = ButtonWidth(repoLabel, icon::Link);
    Button(D2D1::RectF(lx, y, lx + rw, y + 32), repoLabel, icon::Link, Btn::Subtle, Hit::DRepo, g_detail);
    y += 42;
    if (a.inst.installed) {
        g_c.Text(L"Location", Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, left + 140, y + 24), g_t.text);
        g_c.Text(a.inst.dir, Format(13), D2D1::RectF(sx, y, right, y + 24), g_t.subtext);
        y += 32;
    }
    y += 12;

    // changelog
    g_c.Text(L"What's new", Format(18, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, right, y + 30), g_t.text);
    y += 40;
    if (a.releases.empty()) {
        std::wstring t = !a.releasesLoaded ? L"Checking GitHub…" : !a.releaseError.empty() ? a.releaseError : L"No releases published yet.";
        g_c.Text(t, Format(13.5f), D2D1::RectF(left, y, right, y + 24), g_t.subtext);
        y += 40;
    }
    int shown = 0;
    auto* notesF = Format(13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    for (size_t k = 0; k < a.releases.size() && shown < 8; ++k) {
        const ReleaseInfo& r = a.releases[k];
        if (r.prerelease && !UsesBeta(a) && &r != beta) continue;
        ++shown;
        std::wstring notes = CleanNotes(r.notes);
        if (notes.empty()) notes = L"No notes for this release.";
        float nh = MeasureHeight(notes, notesF, right - left - 32);
        float boxH = 56 + nh + 16;
        D2D1_RECT_F box = D2D1::RectF(left, y, right, y + boxH);
        if (box.bottom > 0 && box.top < g_H) {
            g_c.FillRound(box, 10, g_t.surface);
            float hx = left + 16;
            g_c.Text(L"Version " + r.version, Format(15, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(hx, y + 12, right, y + 36), g_t.text);
            float vw = MeasureWidth(L"Version " + r.version, Format(15, DWRITE_FONT_WEIGHT_SEMI_BOLD)) + 10;
            float pxx = hx + vw;
            if (r.prerelease) pxx += Pill(pxx, y + 14, L"Beta", g_t.warn) + 6;
            if (a.inst.installed && CompareVersions(r.version, a.inst.version) == 0) pxx += Pill(pxx, y + 14, L"Installed", g_t.info) + 6;
            g_c.Text(ShortDate(r.published), Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING), D2D1::RectF(hx, y + 12, right - 120, y + 36), g_t.subtext);
            D2D1_RECT_F link = D2D1::RectF(right - 110, y + 10, right - 10, y + 38);
            Button(link, L"On GitHub", 0, Btn::Subtle, Hit::DReleasePage, g_detail, (int)k);
            g_c.Text(notes, notesF, D2D1::RectF(hx, y + 48, right - 16, y + 48 + nh + 4), g_t.subtext);
        }
        y += boxH + 12;
    }
    g_contentH = y + g_scroll + 20;
    g_rt->PopAxisAlignedClip();
}

// ---------------------------------------------------------------- settings
static void Segmented(float x, float y, const std::vector<std::wstring>& opts, int sel, Hit kind) {
    auto* f = Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    float cx = x;
    for (size_t k = 0; k < opts.size(); ++k) {
        float w = MeasureWidth(opts[k], f) + 30;
        D2D1_RECT_F r = D2D1::RectF(cx, y, cx + w, y + 32);
        bool s = (int)k == sel;
        g_c.FillRound(r, 6, s ? Alpha(g_t.accent, 0.18f) : (IsHover(kind, -1, (int)k) ? g_t.surfaceHover : Mix(g_t.surface, g_t.surfaceStrong, 0.3f)));
        if (s) g_c.StrokeRound(r, 6, Alpha(g_t.accent, 0.6f));
        g_c.Text(opts[k], Format(13, s ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER), r, s ? g_t.text : g_t.subtext);
        AddHot(r, kind, -1, (int)k);
        cx += w + 6;
    }
}

static const int kHours[] = {1, 3, 6, 12, 24};

static void DrawSettings(float x0) {
    float pad = 32;
    float left = x0 + pad, right = std::min(g_W - pad, x0 + pad + 820);
    g_viewH = g_H;
    g_rt->PushAxisAlignedClip(D2D1::RectF(x0, 0, g_W, g_H), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = 22 - g_scroll;
    g_c.Text(L"Settings", Format(22, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, right, y + 32), g_t.text);
    y += 52;
    auto section = [&](const std::wstring& title, float h) {
        g_c.Text(title, Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(left, y, right, y + 20), g_t.subtext);
        y += 26;
        D2D1_RECT_F r = D2D1::RectF(left, y, right, y + h);
        g_c.FillRound(r, 10, g_t.surface);
        return r;
    };
    auto rowLabel = [&](float ry, const std::wstring& t, const std::wstring& sub) {
        g_c.Text(t, Format(14), D2D1::RectF(left + 18, ry + 8, right - 380, ry + 28), g_t.text);
        if (!sub.empty()) g_c.Text(sub, Format(12), D2D1::RectF(left + 18, ry + 28, right - 380, ry + 46), g_t.subtext);
    };

    D2D1_RECT_F s1 = section(L"APPEARANCE", 64);
    rowLabel(s1.top + 4, L"Theme", L"More themes and suite-wide theming arrive in Phase 4");
    int th = g_cfg.theme == L"light" ? 1 : g_cfg.theme == L"system" ? 2 : 0;
    Segmented(right - 280, s1.top + 16, {L"Dark", L"Light", L"Windows"}, th, Hit::SetTheme);
    y = s1.bottom + 26;

    D2D1_RECT_F s2 = section(L"UPDATES", 64 * 3);
    rowLabel(s2.top + 4, L"Default channel", L"Each app's page can override this");
    Segmented(right - 200, s2.top + 16, {L"Stable", L"Beta"}, g_cfg.beta ? 1 : 0, Hit::SetChannel);
    g_c.Line(left + 18, s2.top + 64, right - 18, s2.top + 64, g_t.divider);
    rowLabel(s2.top + 68, L"Check for updates every", L"Hangar checks GitHub in the background");
    int hi = 1;
    for (int k = 0; k < 5; ++k) if (kHours[k] == g_cfg.checkHours) hi = k;
    Segmented(right - 300, s2.top + 80, {L"1 h", L"3 h", L"6 h", L"12 h", L"24 h"}, hi, Hit::SetHours);
    g_c.Line(left + 18, s2.top + 128, right - 18, s2.top + 128, g_t.divider);
    rowLabel(s2.top + 132, L"Check now", StatusLine());
    Button(D2D1::RectF(right - 140, s2.top + 144, right - 18, s2.top + 176), L"Check now", icon::Refresh, Btn::Secondary, Hit::SetCheckNow);
    y = s2.bottom + 26;

    D2D1_RECT_F s5 = section(L"HANGAR UPDATES", 64 * 3 + (selfupdate::PreviousVersions().empty() ? 0 : 64));
    {
        std::wstring status;
        switch (g_self.kind) {
            case SelfUpdateState::Checking: status = L"Checking…"; break;
            case SelfUpdateState::UpToDate: status = std::wstring(L"Up to date · ") + HANGAR_VERSION; break;
            case SelfUpdateState::Downloading: status = L"Downloading " + g_self.version + (g_self.percent >= 0 ? L" · " + std::to_wstring(g_self.percent) + L"%" : L""); break;
            case SelfUpdateState::Ready: status = L"Hangar " + g_self.version + L" is downloaded and verified"; break;
            case SelfUpdateState::Error: status = g_self.message; break;
            default: status = std::wstring(L"Version ") + HANGAR_VERSION; break;
        }
        rowLabel(s5.top + 4, std::wstring(L"Hangar ") + HANGAR_VERSION, status);
        if (g_self.kind == SelfUpdateState::Ready)
            Button(D2D1::RectF(right - 170, s5.top + 16, right - 18, s5.top + 48), L"Restart to update", icon::Update, Btn::Primary, Hit::SelfRestart);
        else
            Button(D2D1::RectF(right - 170, s5.top + 16, right - 18, s5.top + 48), L"Check now", icon::Refresh, Btn::Secondary, Hit::SelfCheck, -1, 0,
                   g_self.kind != SelfUpdateState::Checking && g_self.kind != SelfUpdateState::Downloading);
        g_c.Line(left + 18, s5.top + 64, right - 18, s5.top + 64, g_t.divider);
        rowLabel(s5.top + 68, L"Hangar's channel", L"Beta gets new Hangar features first");
        Segmented(right - 200, s5.top + 80, {L"Stable", L"Beta"}, g_cfg.hangarBeta ? 1 : 0, Hit::SelfChannel);
        g_c.Line(left + 18, s5.top + 128, right - 18, s5.top + 128, g_t.divider);
        rowLabel(s5.top + 132, L"Install Hangar updates automatically", L"Only when nothing is installing and you're not using Hangar");
        Segmented(right - 200, s5.top + 144, {L"On", L"Off"}, g_cfg.hangarAuto ? 0 : 1, Hit::SelfAuto);
        auto prev = selfupdate::PreviousVersions();
        if (!prev.empty()) {
            g_c.Line(left + 18, s5.top + 192, right - 18, s5.top + 192, g_t.divider);
            rowLabel(s5.top + 196, L"Previous versions", L"Go back if a new Hangar gives you trouble");
            float bx = right - 18;
            for (size_t k = 0; k < prev.size() && k < 2; ++k) {
                std::wstring l = L"Roll back to " + prev[k].version;
                float w = ButtonWidth(l, 0);
                Button(D2D1::RectF(bx - w, s5.top + 208, bx, s5.top + 240), l, 0, Btn::Secondary, Hit::SelfRollback, -1, (int)k);
                bx -= w + 8;
            }
        }
    }
    y = s5.bottom + 26;

    D2D1_RECT_F s3 = section(L"ADVANCED", 64 * 3);
    rowLabel(s3.top + 4, L"Data folder", DataDir());
    Button(D2D1::RectF(right - 140, s3.top + 16, right - 18, s3.top + 48), L"Open", icon::Folder, Btn::Secondary, Hit::SetOpenData);
    g_c.Line(left + 18, s3.top + 64, right - 18, s3.top + 64, g_t.divider);
    rowLabel(s3.top + 68, L"Log", L"hangar.log, for troubleshooting");
    Button(D2D1::RectF(right - 140, s3.top + 80, right - 18, s3.top + 112), L"Open log", 0, Btn::Secondary, Hit::SetOpenLog);
    g_c.Line(left + 18, s3.top + 128, right - 18, s3.top + 128, g_t.divider);
    rowLabel(s3.top + 132, L"Local manifest", L"Add apps that aren't public yet (manifest.local.json)");
    Button(D2D1::RectF(right - 140, s3.top + 144, right - 18, s3.top + 176), L"Edit", 0, Btn::Secondary, Hit::SetLocalManifest);
    y = s3.bottom + 26;


    D2D1_RECT_F s4 = section(L"ABOUT", 64);
    rowLabel(s4.top + 4, std::wstring(L"Hangar ") + HANGAR_VERSION + (IsPortable() ? L" (portable)" : L""), L"larosa023");
    Button(D2D1::RectF(right - 180, s4.top + 16, right - 18, s4.top + 48), L"github.com/larosa023/Hangar", 0, Btn::Subtle, Hit::SetRepo);
    y = s4.bottom + 26;
    g_contentH = y + g_scroll + 10;
    g_rt->PopAxisAlignedClip();
}

// ---------------------------------------------------------------- toast
static void DrawToast() {
    if (g_toastAnim <= 0.01f || g_toast.empty()) return;
    auto* f = Format(13.5f);
    float w = std::min(g_W - kSidebar - 60, MeasureWidth(g_toast, f) + 70);
    float cx = kSidebar + (g_W - kSidebar) / 2;
    float y = g_H - 20 - 44 * g_toastAnim;
    D2D1_RECT_F r = D2D1::RectF(cx - w / 2, y, cx + w / 2, y + 40);
    g_c.FillRound(r, 8, Alpha(g_t.dark ? Rgb(0x2E2E36) : Rgb(0x26262C), g_toastAnim));
    g_c.Glyph(g_toastBad ? icon::Warning : icon::Check, Format(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true),
              D2D1::RectF(r.left + 8, r.top, r.left + 36, r.bottom), Alpha(g_toastBad ? Rgb(0xFF8A8A) : Rgb(0x7BE0A6), g_toastAnim));
    g_c.Text(g_toast, f, D2D1::RectF(r.left + 38, r.top, r.right - 30, r.bottom), Alpha(Rgb(0xF4F4F6), g_toastAnim));
    D2D1_RECT_F x = D2D1::RectF(r.right - 30, r.top + 8, r.right - 6, r.bottom - 8);
    g_c.Glyph(icon::Cancel, Format(9, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), x, Alpha(Rgb(0xB0B0BA), g_toastAnim));
    AddHot(r, Hit::ToastClose);
}

// ---------------------------------------------------------------- what's new (after a self-update)
static void DrawWhatsNew() {
    if (g_whatsNew.empty()) return;
    g_hot.clear();   // the overlay is modal
    g_c.Fill(D2D1::RectF(0, 0, g_W, g_H), Rgb(0x000000, 0.45f));
    AddHot(D2D1::RectF(0, 0, g_W, g_H), Hit::WhatsNewClose);
    float w = std::min(560.0f, g_W - 80);
    auto* f = Format(13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    float th = std::min(MeasureHeight(g_whatsNew, f, w - 56), g_H - 260);
    float h = th + 150;
    D2D1_RECT_F r = D2D1::RectF((g_W - w) / 2, (g_H - h) / 2, (g_W + w) / 2, (g_H + h) / 2);
    g_c.FillRound(r, 12, g_t.dark ? Rgb(0x1F1F25) : Rgb(0xFFFFFF));
    g_c.StrokeRound(r, 12, g_t.divider);
    AddHot(r, Hit::None);
    g_c.Glyph(icon::Star, Format(18, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, true), D2D1::RectF(r.left + 22, r.top + 22, r.left + 50, r.top + 50), g_t.accent);
    g_c.Text(L"Hangar updated to " + g_whatsNewVersion, Format(19, DWRITE_FONT_WEIGHT_SEMI_BOLD), D2D1::RectF(r.left + 56, r.top + 20, r.right - 24, r.top + 52), g_t.text);
    g_c.Text(L"You're right where you left off.", Format(12.5f), D2D1::RectF(r.left + 56, r.top + 50, r.right - 24, r.top + 70), g_t.subtext);
    g_rt->PushAxisAlignedClip(D2D1::RectF(r.left + 28, r.top + 84, r.right - 28, r.top + 84 + th), D2D1_ANTIALIAS_MODE_ALIASED);
    g_c.Text(g_whatsNew, f, D2D1::RectF(r.left + 28, r.top + 84, r.right - 28, r.top + 84 + th + 4), g_t.subtext);
    g_rt->PopAxisAlignedClip();
    Button(D2D1::RectF(r.right - 120, r.bottom - 50, r.right - 24, r.bottom - 18), L"Got it", 0, Btn::Primary, Hit::WhatsNewClose);
}

// ---------------------------------------------------------------- paint
static void ClampScroll() {
    float maxS = std::max(0.0f, g_contentH - g_viewH);
    g_scrollTarget = std::clamp(g_scrollTarget, 0.0f, maxS);
    g_scroll = std::clamp(g_scroll, 0.0f, maxS);
}

static void DrawScrollbar(float x0) {
    float top = g_page == Page::Library ? kHeader : 0;
    if (g_contentH <= g_viewH + 1) return;
    float trackH = g_viewH - 8;
    float thumbH = std::max(40.0f, trackH * g_viewH / g_contentH);
    float t = g_scroll / (g_contentH - g_viewH);
    float y = top + 4 + (trackH - thumbH) * t;
    g_c.FillRound(D2D1::RectF(g_W - 8, y, g_W - 4, y + thumbH), 2, g_t.dark ? Rgb(0xFFFFFF, 0.18f) : Rgb(0x000000, 0.2f));
    (void)x0;
}

static void Paint() {
    if (!EnsureDevice()) return;
    D2D1_SIZE_F sz = g_rt->GetSize();
    g_W = sz.width;
    g_H = sz.height;
    g_c.rt = g_rt;
    g_c.br = g_br;
    g_hot.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    g_rt->Clear(g_t.bg);
    float x0 = kSidebar;
    // page transition: content fades and slides in slightly
    float slide = (1.0f - g_pageAnim) * 14;
    if (slide > 0.05f) g_rt->SetTransform(D2D1::Matrix3x2F::Translation(0, slide));
    if (g_page == Page::Library) DrawLibrary(x0);
    else if (g_page == Page::Details) DrawDetails(x0);
    else DrawSettings(x0);
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (g_pageAnim < 0.999f) g_c.Fill(D2D1::RectF(x0, 0, g_W, g_H), Alpha(Rgb(g_t.dark ? 0x16161A : 0xF5F5F7), (1.0f - g_pageAnim) * 0.85f));
    DrawScrollbar(x0);
    DrawSidebar();
    DrawToast();
    DrawWhatsNew();
    HRESULT hr = g_rt->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) DiscardDevice();
    ClampScroll();
}

// ---------------------------------------------------------------- animation
static bool Animate() {
    bool more = false;
    // smooth scrolling
    if (std::fabs(g_scrollTarget - g_scroll) > 0.5f) { g_scroll += (g_scrollTarget - g_scroll) * 0.28f; more = true; }
    else g_scroll = g_scrollTarget;
    // hover fades
    for (size_t i = 0; i < g_apps.size(); ++i) {
        AppState& a = g_apps[i];
        bool hot = (g_hover.kind == Hit::Card || g_hover.kind == Hit::CardAction) && g_hover.index == (int)i;
        float target = hot ? 1.0f : 0.0f;
        if (std::fabs(a.hover - target) > 0.01f) { a.hover += (target - a.hover) * 0.3f; more = true; }
        else a.hover = target;
        if (a.busy) {
            if (a.percent < 0) more = true;   // indeterminate bar
            else if (std::fabs(a.shownPercent - a.percent) > 0.2f) { a.shownPercent += (a.percent - a.shownPercent) * 0.25f; more = true; }
            else a.shownPercent = (float)a.percent;
        }
    }
    if (g_pageAnim < 1.0f) { g_pageAnim = std::min(1.0f, g_pageAnim + 0.12f); more = true; }
    float toastTarget = (!g_toast.empty() && GetTickCount() - g_toastAt < 60000) ? 1.0f : 0.0f;
    if (std::fabs(g_toastAnim - toastTarget) > 0.01f) { g_toastAnim += (toastTarget - g_toastAnim) * 0.25f; more = true; }
    else { g_toastAnim = toastTarget; if (toastTarget == 0) g_toast.clear(); }
    return more;
}

// ---------------------------------------------------------------- navigation
static void Navigate(Page p, int detail = -1) {
    g_savedScroll[g_page] = g_scrollTarget;
    g_page = p;
    g_detail = detail;
    g_scroll = g_scrollTarget = (p == Page::Library) ? g_savedScroll[Page::Library] : 0;
    g_pageAnim = 0.0f;
    g_searchFocus = false;
    StartAnim();
}

static void ShowNav(const std::wstring& id) {
    if (id == L"settings") { Navigate(Page::Settings); return; }
    if (g_page == Page::Library && g_nav == id) return;
    g_nav = id;
    g_cfg.page = id;
    g_focus = -1;
    g_savedScroll[Page::Library] = 0;
    Navigate(Page::Library);
}

static void EnsureFocusVisible() {
    if (g_focus < 0) return;
    float rowH = g_cfg.listView ? 70.0f : 164.0f;
    int row = g_cfg.listView ? g_focus : g_focus / std::max(1, g_cols);
    float top = row * rowH, bottom = top + rowH;
    if (top < g_scrollTarget) g_scrollTarget = top;
    else if (bottom > g_scrollTarget + g_viewH - 20) g_scrollTarget = bottom - g_viewH + 20;
    ClampScroll();
    StartAnim();
}

// ---------------------------------------------------------------- input
static HotRect HitAt(float x, float y) {
    for (auto it = g_hot.rbegin(); it != g_hot.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) {
            // anything scrolled under the library header isn't clickable
            if (g_page == Page::Library && y < kHeader && x > kSidebar &&
                (it->kind == Hit::Card || it->kind == Hit::CardAction)) continue;
            return *it;
        }
    return {{0, 0, 0, 0}, Hit::None, -1, 0, false};
}

// ---------------------------------------------------------------- Hangar's own updates
static void SaveWindowPlacement();

static std::wstring RestoreJson() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"page", json::Value::Str(g_page == Page::Details ? L"details" : g_page == Page::Settings ? L"settings" : L"library"));
    v.Set(L"nav", json::Value::Str(g_nav));
    v.Set(L"detail", json::Value::Str(g_detail >= 0 && g_detail < (int)g_apps.size() ? g_apps[g_detail].def.id : L""));
    v.Set(L"search", json::Value::Str(g_search));
    v.Set(L"scroll", json::Value::Num(g_scrollTarget));
    v.Set(L"listView", json::Value::Boolean(g_cfg.listView));
    v.Set(L"background", json::Value::Boolean(GetForegroundWindow() != g_hwnd));
    v.Set(L"minimized", json::Value::Boolean(IsIconic(g_hwnd) != 0));
    std::wstring out;
    json::Write(v, out);
    return out;
}

// Restarts into a newer (or, for a rollback, an older) Hangar. Never while apps are installing.
static void ApplySelf(const std::wstring& exe, const std::wstring& version, bool userAsked) {
    if (jobs::ActiveOps() > 0) {
        g_applyWhenIdle = true;
        if (userAsked) Toast(L"Hangar will restart once the current installs finish.");
        return;
    }
    g_applyWhenIdle = false;
    SaveWindowPlacement();
    ConfigSave();
    selfupdate::SaveRestoreState(RestoreJson());
    Log(L"self-update: restarting into %ls", version.c_str());
    std::wstring err;
    if (selfupdate::Apply(g_hwnd, exe, version, &err)) {
        jobs::Shutdown();
        DestroyWindow(g_hwnd);   // the new copy has taken over
        return;
    }
    DeleteFileW((DataDir() + L"\\restore.json").c_str());
    if (IStartsWith(err, L"Hangar " + version + L" didn't start")) {
        SkipVersion(version);
        ConfigSave();
        g_self = SelfUpdateState{};
        g_self.kind = SelfUpdateState::Error;
        g_self.message = err;
    }
    Toast(err, true);
    Log(L"self-update: %ls", err.c_str());
}

static void MaybeAutoApplySelf() {
    if (g_self.kind != SelfUpdateState::Ready || jobs::ActiveOps() > 0) return;
    if (!g_cfg.hangarAuto && !g_applyWhenIdle) return;
    if (!g_whatsNew.empty()) return;
    // only when it can't interrupt: Hangar is minimized, or nobody has touched the PC for 2 minutes
    LASTINPUTINFO li{sizeof(li)};
    bool idle = GetLastInputInfo(&li) && GetTickCount() - li.dwTime > 2 * 60 * 1000;
    bool away = IsIconic(g_hwnd) || idle;
    if (g_applyWhenIdle || away) ApplySelf(g_self.staged, g_self.version, false);
}

static void CheckSelf(bool force) {
    g_selfCheckedTick = GetTickCount();
    selfupdate::Check(g_hwnd, force);
}

static void Click(const HotRect& h) {
    if (!h.enabled) return;
    switch (h.kind) {
        case Hit::Nav: if (h.arg >= 0 && h.arg < (int)g_navIds.size()) ShowNav(g_navIds[h.arg]); break;
        case Hit::Search: g_searchFocus = true; g_caretOn = true; SetTimer(g_hwnd, TIMER_CARET, 530, nullptr); break;
        case Hit::SearchClear: g_search.clear(); g_focus = -1; g_scrollTarget = 0; break;
        case Hit::ViewGrid: g_cfg.listView = false; g_scrollTarget = 0; ConfigSave(); break;
        case Hit::ViewList: g_cfg.listView = true; g_scrollTarget = 0; ConfigSave(); break;
        case Hit::Refresh: CheckReleases(true); ScanAll(); jobs::FetchManifest(g_manifestText); break;
        case Hit::UpdateAll: UpdateAll(); break;
        case Hit::Card: Navigate(Page::Details, h.index); break;
        case Hit::CardAction: DoPrimary(h.index); break;
        case Hit::Back: Navigate(Page::Library); break;
        case Hit::DAction:
            if (h.arg == 1) Launch(g_apps[h.index]);
            else DoPrimary(h.index);
            break;
        case Hit::DCancel: jobs::CancelOp(g_apps[h.index].def.id); g_apps[h.index].opText = L"Cancelling…"; break;
        case Hit::DOpenFolder: OpenFolder(g_apps[h.index]); break;
        case Hit::DRepair: Repair(h.index); break;
        case Hit::DUninstall: Uninstall(h.index); break;
        case Hit::DLocate: Locate(h.index); break;
        case Hit::DChannel: SetAppChannel(g_apps[h.index].def.id, h.arg == 1); ConfigSave(); break;
        case Hit::DReleasePage:
            if (h.arg >= 0 && h.arg < (int)g_apps[h.index].releases.size()) OpenUrl(g_apps[h.index].releases[h.arg].page);
            break;
        case Hit::DRepo: OpenUrl(L"https://github.com/" + g_apps[h.index].def.repo); break;
        case Hit::SetTheme: {
            static const wchar_t* names[] = {L"dark", L"light", L"system"};
            g_cfg.theme = names[h.arg];
            ConfigSave();
            g_t = BuildTheme(g_cfg.theme);
            ApplyWindowTheme(g_hwnd, g_t);
            break;
        }
        case Hit::SetChannel: g_cfg.beta = h.arg == 1; ConfigSave(); break;
        case Hit::SetHours: g_cfg.checkHours = kHours[h.arg]; ConfigSave(); break;
        case Hit::SetCheckNow: CheckReleases(true); jobs::FetchManifest(g_manifestText); break;
        case Hit::SetOpenData: ShellExecuteW(g_hwnd, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case Hit::SetOpenLog: ShellExecuteW(g_hwnd, L"open", LogPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case Hit::SetLocalManifest: {
            std::wstring p = DataDir() + L"\\manifest.local.json";
            if (!FileExists(p)) {
                WriteTextFileAtomic(p,
                    L"{\n  \"schema\": 1,\n  \"apps\": [\n    {\n      \"id\": \"myapp\",\n      \"name\": \"MyApp\",\n"
                    L"      \"repo\": \"larosa023/MyApp\",\n      \"description\": \"An app I'm testing.\",\n      \"category\": \"work\",\n"
                    L"      \"exe\": \"MyApp.exe\",\n      \"install\": { \"type\": \"portable\", \"asset\": \"MyApp.exe\" }\n    }\n  ]\n}\n");
            }
            ShellExecuteW(g_hwnd, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            Toast(L"Save the file, then press Refresh to load it.");
            break;
        }
        case Hit::SetRepo: OpenUrl(L"https://github.com/larosa023/Hangar"); break;
        case Hit::ToastClose: g_toastAt = 0; break;
        case Hit::SelfRestart: if (g_self.kind == SelfUpdateState::Ready) ApplySelf(g_self.staged, g_self.version, true); break;
        case Hit::SelfCheck: CheckSelf(true); break;
        case Hit::SelfChannel:
            g_cfg.hangarBeta = h.arg == 1;
            ConfigSave();
            CheckSelf(true);
            break;
        case Hit::SelfAuto: g_cfg.hangarAuto = h.arg == 0; ConfigSave(); break;
        case Hit::SelfRollback: {
            auto prev = selfupdate::PreviousVersions();
            if (h.arg < 0 || h.arg >= (int)prev.size()) break;
            std::wstring msg = L"Go back to Hangar " + prev[h.arg].version + L"?\n\nHangar restarts and won't install " + HANGAR_VERSION +
                               L" again automatically. Your apps and settings aren't touched.";
            if (MessageBoxW(g_hwnd, msg.c_str(), L"Roll back Hangar", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) break;
            SkipVersion(HANGAR_VERSION);
            ApplySelf(prev[h.arg].exe, prev[h.arg].version, true);
            break;
        }
        case Hit::WhatsNewClose: g_whatsNew.clear(); break;
        default: break;
    }
    if (h.kind != Hit::Search && h.kind != Hit::SearchClear) g_searchFocus = false;
    StartAnim();
}

static void Scroll(float delta) {
    g_scrollTarget -= delta;
    ClampScroll();
    StartAnim();
}

static void OnKey(WPARAM vk) {
    g_lastInput = GetTickCount();
    if (!g_whatsNew.empty()) {
        if (vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_SPACE) { g_whatsNew.clear(); Invalidate(); }
        return;
    }
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    bool alt = GetKeyState(VK_MENU) < 0;
    if (ctrl && (vk == 'K' || vk == 'F')) {
        if (g_page != Page::Library) Navigate(Page::Library);
        g_searchFocus = true;
        g_caretOn = true;
        SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
        Invalidate();
        return;
    }
    if (ctrl && vk == 'U') { UpdateAll(); return; }
    if (vk == VK_F5 || (ctrl && vk == 'R')) { CheckReleases(true); ScanAll(); return; }
    if (ctrl && vk == VK_OEM_COMMA) { Navigate(Page::Settings); return; }
    if ((alt && vk == VK_LEFT) || vk == VK_BROWSER_BACK) { if (g_page != Page::Library) Navigate(Page::Library); return; }

    if (g_searchFocus) {
        if (vk == VK_ESCAPE) {
            if (!g_search.empty()) { g_search.clear(); g_scrollTarget = 0; }
            else g_searchFocus = false;
            Invalidate();
            return;
        }
        if (vk == VK_DOWN || vk == VK_RETURN) {
            g_searchFocus = false;
            g_keyboardMode = true;
            ComputeVisible();
            if (!g_visible.empty()) {
                g_focus = 0;
                if (vk == VK_RETURN && g_visible.size() == 1) Navigate(Page::Details, g_visible[0]);
            }
            Invalidate();
            return;
        }
        return;   // characters arrive in WM_CHAR
    }

    if (g_page == Page::Details) {
        if (vk == VK_ESCAPE || vk == VK_BACK) Navigate(Page::Library);
        else if (vk == VK_RETURN) DoPrimary(g_detail);
        else if (vk == VK_DOWN) Scroll(-60);
        else if (vk == VK_UP) Scroll(60);
        else if (vk == VK_NEXT) Scroll(-(g_viewH - 80));
        else if (vk == VK_PRIOR) Scroll(g_viewH - 80);
        return;
    }
    if (g_page == Page::Settings) {
        if (vk == VK_ESCAPE || vk == VK_BACK) Navigate(Page::Library);
        else if (vk == VK_DOWN) Scroll(-60);
        else if (vk == VK_UP) Scroll(60);
        return;
    }
    // library
    ComputeVisible();
    int n = (int)g_visible.size();
    if (vk == VK_ESCAPE) {
        if (!g_search.empty()) { g_search.clear(); g_scrollTarget = 0; Invalidate(); }
        return;
    }
    if (!n) return;
    int cols = g_cfg.listView ? 1 : std::max(1, g_cols);
    int f = g_focus;
    switch (vk) {
        case VK_RIGHT: f = f < 0 ? 0 : std::min(n - 1, f + 1); break;
        case VK_LEFT: f = f < 0 ? 0 : std::max(0, f - 1); break;
        case VK_DOWN: f = f < 0 ? 0 : std::min(n - 1, f + cols); break;
        case VK_UP: f = f < 0 ? 0 : std::max(0, f - cols); break;
        case VK_HOME: f = 0; break;
        case VK_END: f = n - 1; break;
        case VK_RETURN: if (f >= 0 && f < n) Navigate(Page::Details, g_visible[f]); return;
        case VK_SPACE: if (f >= 0 && f < n) DoPrimary(g_visible[f]); return;
        default: return;
    }
    g_focus = f;
    g_keyboardMode = true;
    EnsureFocusVisible();
    Invalidate();
}

static void OnChar(wchar_t c) {
    if (c < 32 || c == 127) {
        if (c == 8 && g_searchFocus && !g_search.empty()) {   // Backspace
            if (GetKeyState(VK_CONTROL) < 0) {
                size_t p = g_search.find_last_of(L' ', g_search.size() - 1);
                g_search = p == std::wstring::npos ? L"" : g_search.substr(0, p);
            } else g_search.pop_back();
            g_focus = -1;
            g_scrollTarget = 0;
            Invalidate();
        } else if (c == 22 && g_searchFocus) {                  // Ctrl+V
            if (OpenClipboard(g_hwnd)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* p = (const wchar_t*)GlobalLock(h)) { g_search += Trim(p).substr(0, 64); GlobalUnlock(h); }
                }
                CloseClipboard();
                Invalidate();
            }
        }
        return;
    }
    if (g_page != Page::Library) return;
    if (!g_searchFocus) {
        if (c == L' ') return;   // Space is "primary action" in the library
        g_searchFocus = true;
        SetTimer(g_hwnd, TIMER_CARET, 530, nullptr);
    }
    if (g_search.size() < 64) g_search += c;
    g_caretOn = true;
    g_focus = -1;
    g_scrollTarget = 0;
    Invalidate();
}

// ---------------------------------------------------------------- messages from workers
static void OnReleases(ReleasesResult* r) {
    AppState* a = FindApp(r->id);
    if (!a) return;
    if (!r->fromCache) a->checking = false;
    if (r->ok || !r->releases.empty()) {
        a->releases = std::move(r->releases);
        a->releasesLoaded = true;
    }
    if (!r->fromCache) {
        a->releasesLoaded = true;
        a->releaseError = r->error;
        g_offline = !r->error.empty() && (IContains(r->error, L"online") || IContains(r->error, L"connect"));
    }
    if (r->checkedAt) a->checkedAt = r->checkedAt;
}

static void OnScan(ScanResult* r) {
    for (auto& it : r->items) {
        if (AppState* a = FindApp(it.first)) {
            if (a->busy) continue;   // the op will rescan when it's done
            a->inst = it.second;
            a->scanned = true;
        }
    }
    PollProcesses();
}

static bool OnProcs(ProcResult* r) {
    bool changed = false;
    for (size_t i = 0; i < r->ids.size(); ++i)
        if (AppState* a = FindApp(r->ids[i])) {
            std::vector<DWORD> p = r->pids[i];
            std::sort(p.begin(), p.end());
            if (p != a->pids) { a->pids = std::move(p); changed = true; }
        }
    return changed;
}

static void OnOpProgress(OpProgress* p) {
    AppState* a = FindApp(p->id);
    if (!a || !a->busy) return;
    if (p->percent >= 0 && a->percent < 0) a->shownPercent = (float)p->percent;
    a->percent = p->percent;
    if (!(a->opText == L"Cancelling…" && p->percent >= 0)) a->opText = p->text;
    if (p->percent >= 0 && a->opText.find(L"Downloading") == 0 && p->percent >= 0) {
        // keep the percent in the text for the list and app page
        a->opText = p->text + L"  ·  " + std::to_wstring(p->percent) + L"%";
    }
    StartAnim();
}

static void OnOpDone(OpDone* d) {
    AppState* a = FindApp(d->id);
    if (!a) return;
    a->busy = false;
    a->percent = -1;
    if (d->ok) {
        a->lastError.clear();
        if (d->kind == OpKind::Uninstall) ForgetLocation(a->def.id);
        Toast(d->message);
    } else if (d->cancelled) {
        a->lastError.clear();
        Toast(a->def.name + L": cancelled.");
    } else {
        a->lastError = d->message;
        Toast(a->def.name + L": " + d->message, true);
    }
    if (g_applyWhenIdle && jobs::ActiveOps() == 0) SetTimer(g_hwnd, TIMER_SELF, 1500, nullptr);
    // see what actually changed on disk
    jobs::Scan({a->def});
}

// ---------------------------------------------------------------- window
static void SaveWindowPlacement() {
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    g_cfg.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
    RECT r = wp.rcNormalPosition;
    g_cfg.windowX = r.left;
    g_cfg.windowY = r.top;
    g_cfg.windowW = r.right - r.left;
    g_cfg.windowH = r.bottom - r.top;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            g_scale = GetDpiForWindow(hwnd) / 96.0f;
            g_t = BuildTheme(g_cfg.theme);
            ApplyWindowTheme(hwnd, g_t);
            jobs::Init(hwnd);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            if (wp == SIZE_MINIMIZED) KillTimer(hwnd, TIMER_PROCS);
            else SetTimer(hwnd, TIMER_PROCS, 2500, nullptr);
            Invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            float s = GetDpiForWindow(hwnd) / 96.0f;
            mm->ptMinTrackSize.x = (LONG)(820 * s);
            mm->ptMinTrackSize.y = (LONG)(540 * s);
            return 0;
        }
        case WM_DPICHANGED: {
            g_scale = HIWORD(wp) / 96.0f;
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (g_rt) g_rt->SetDpi((float)HIWORD(wp), (float)HIWORD(wp));
            Invalidate();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && (wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0)) {
                g_t = BuildTheme(g_cfg.theme);
                ApplyWindowTheme(hwnd, g_t);
                Invalidate();
            }
            return 0;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            g_t = BuildTheme(g_cfg.theme);
            Invalidate();
            return 0;
        case WM_MOUSEMOVE: {
            g_lastInput = GetTickCount();
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            if (!g_tracking) {
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&t);
                g_tracking = true;
            }
            HotRect h = HitAt(x, y);
            if (h.kind != g_hover.kind || h.index != g_hover.index || h.arg != g_hover.arg) {
                g_hover = h;
                StartAnim();
            }
            if (g_keyboardMode) { g_keyboardMode = false; Invalidate(); }
            bool hand = h.kind != Hit::None && h.enabled && h.kind != Hit::Search;
            SetCursor(LoadCursorW(nullptr, h.kind == Hit::Search ? IDC_IBEAM : hand ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;   // set in WM_MOUSEMOVE
            break;
        case WM_MOUSELEAVE:
            g_tracking = false;
            g_hover = {{0, 0, 0, 0}, Hit::None, -1, 0, false};
            StartAnim();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            g_pressed = HitAt(x, y);
            if (g_pressed.kind == Hit::None && g_searchFocus) { g_searchFocus = false; }
            SetCapture(hwnd);
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            float x = GET_X_LPARAM(lp) / g_scale, y = GET_Y_LPARAM(lp) / g_scale;
            HotRect up = HitAt(x, y);
            HotRect down = g_pressed;
            g_pressed = {{0, 0, 0, 0}, Hit::None, -1, 0, false};
            if (up.kind == down.kind && up.index == down.index && up.arg == down.arg && up.kind != Hit::None) Click(up);
            Invalidate();
            return 0;
        }
        case WM_XBUTTONUP:
            if (GET_XBUTTON_WPARAM(wp) == XBUTTON1 && g_page != Page::Library) Navigate(Page::Library);
            return TRUE;
        case WM_MOUSEWHEEL:
            Scroll(GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 96.0f);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (msg == WM_SYSKEYDOWN && wp != VK_LEFT) break;
            OnKey(wp);
            return 0;
        case WM_CHAR:
            OnChar((wchar_t)wp);
            return 0;
        case WM_TIMER:
            switch (wp) {
                case TIMER_ANIM:
                    if (!Animate()) { KillTimer(hwnd, TIMER_ANIM); g_animTimer = false; }
                    Invalidate();
                    break;
                case TIMER_CARET:
                    if (!g_searchFocus || GetFocus() != hwnd) { KillTimer(hwnd, TIMER_CARET); g_caretOn = false; }
                    else g_caretOn = !g_caretOn;
                    Invalidate();
                    break;
                case TIMER_PROCS:
                    SetTimer(hwnd, TIMER_PROCS, 2500, nullptr);
                    if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) PollProcesses();
                    break;
                case TIMER_CHECK:
                    // the jobs only touch the network when the cache is older than the check interval
                    CheckReleases(false);
                    jobs::FetchManifest(g_manifestText);
                    break;
                case TIMER_SELF:
                    SetTimer(hwnd, TIMER_SELF, 20 * 1000, nullptr);
                    if (GetTickCount() - g_selfCheckedTick > (DWORD)g_cfg.checkHours * 3600 * 1000 || !g_selfCheckedTick) CheckSelf(false);
                    MaybeAutoApplySelf();
                    break;
                case TIMER_HEALTHY:
                    // the window is up and drawn: tell the copy we replaced that this version works
                    KillTimer(hwnd, TIMER_HEALTHY);
                    selfupdate::SignalHealthy(g_after);
                    selfupdate::FinishAfterUpdate(g_after);
                    break;
                case TIMER_TOAST:
                    KillTimer(hwnd, TIMER_TOAST);
                    g_toastAt = 0;
                    StartAnim();
                    break;
            }
            return 0;
        case WM_ACTIVATEAPP:
            // pick up apps that updated or uninstalled themselves while Hangar was in the background
            if (wp && GetTickCount() - g_lastScan > 8000) ScanAll();
            return 0;
        case WM_KILLFOCUS:
            g_searchFocus = false;
            Invalidate();
            return 0;
        case WM_HANGAR_MANIFEST: {
            std::unique_ptr<ManifestResult> r((ManifestResult*)lp);
            if (r->changed) { ApplyManifest(r->text, true); Toast(L"The app list was updated."); }
            if (!r->error.empty() && g_apps.empty()) g_manifestError = r->error;
            Invalidate();
            return 0;
        }
        case WM_HANGAR_RELEASES: { std::unique_ptr<ReleasesResult> r((ReleasesResult*)lp); OnReleases(r.get()); Invalidate(); return 0; }
        case WM_HANGAR_ICON: {
            std::unique_ptr<IconResult> r((IconResult*)lp);
            if (AppState* a = FindApp(r->id)) {
                SafeRelease(a->icon);
                a->iconPixels = std::move(r->pixels);
                a->iconW = r->w;
                a->iconH = r->h;
                Invalidate();
            }
            return 0;
        }
        case WM_HANGAR_SCAN: { std::unique_ptr<ScanResult> r((ScanResult*)lp); OnScan(r.get()); Invalidate(); return 0; }
        case WM_HANGAR_PROCS: { std::unique_ptr<ProcResult> r((ProcResult*)lp); if (OnProcs(r.get())) Invalidate(); return 0; }
        case WM_HANGAR_OP_PROGRESS: { std::unique_ptr<OpProgress> r((OpProgress*)lp); OnOpProgress(r.get()); return 0; }
        case WM_HANGAR_OP_DONE: { std::unique_ptr<OpDone> r((OpDone*)lp); OnOpDone(r.get()); StartAnim(); return 0; }
        case WM_HANGAR_SELF: {
            std::unique_ptr<SelfUpdateState> r((SelfUpdateState*)lp);
            bool wasReady = g_self.kind == SelfUpdateState::Ready;
            // a failed background check doesn't hide an update that's already waiting
            if (!(wasReady && (r->kind == SelfUpdateState::Error || r->kind == SelfUpdateState::Checking))) g_self = *r;
            if (r->kind == SelfUpdateState::Ready && !wasReady) {
                if (!g_cfg.hangarAuto) Toast(L"Hangar " + r->version + L" is ready. Restart Hangar to update.");
                MaybeAutoApplySelf();
            }
            Invalidate();
            return 0;
        }
        case WM_HANGAR_ACTIVATE:
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return 0;
        case WM_CLOSE:
            if (int n = jobs::ActiveOps()) {
                std::wstring m = n == 1 ? L"Hangar is still installing an app. Quit anyway and cancel it?"
                                        : L"Hangar is still installing " + std::to_wstring(n) + L" apps. Quit anyway and cancel them?";
                if (MessageBoxW(hwnd, m.c_str(), L"Hangar", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return 0;
            }
            SaveWindowPlacement();
            ConfigSave();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            jobs::Shutdown();
            DiscardDevice();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND UiCreate(HINSTANCE inst, int show, const selfupdate::AfterUpdate& after) {
    g_after = after;
    if (!gfx::Init()) return nullptr;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"Hangar.larosa023.Main";
    RegisterClassExW(&wc);

    // the library shows immediately from the cached (or built-in) manifest
    std::wstring text;
    if (!ReadTextFile(CacheDir() + L"\\manifest.json", text) || !ParseManifest(text, g_manifest, nullptr)) {
        if (!LoadResourceText(100, text)) text.clear();
    }
    g_nav = g_cfg.page;

    UINT dpi = GetDpiForSystem();
    float s = dpi / 96.0f;
    int w = g_cfg.windowW > 0 ? g_cfg.windowW : (int)(1180 * s);
    int h = g_cfg.windowH > 0 ? g_cfg.windowH : (int)(780 * s);
    int x = g_cfg.windowX, y = g_cfg.windowY;
    if (x != CW_USEDEFAULT) {
        // keep the window on a monitor that still exists
        RECT r{x, y, x + w, y + h};
        HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        if (!mon) x = y = CW_USEDEFAULT;
    }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Hangar", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return nullptr;
    if (!text.empty()) ApplyManifest(text, false);
    // check that "all/installed/updates/<category>" still exists
    bool navOk = g_nav == L"all" || g_nav == L"installed" || g_nav == L"updates";
    for (auto& c : g_manifest.categories) navOk |= c.id == g_nav;
    if (!navOk) g_nav = L"all";
    PruneSkippedVersions(HANGAR_VERSION);
    if (after.active) {
        // a Hangar update just handed over: open where the previous copy was
        std::wstring text = selfupdate::TakeRestoreState();
        json::Value v;
        if (!text.empty() && json::Parser(text).Parse(v) && v.type == json::Value::Object) {
            g_nav = v.GetStr(L"nav", g_nav);
            g_search = v.GetStr(L"search");
            std::wstring page = v.GetStr(L"page");
            std::wstring detail = v.GetStr(L"detail");
            for (size_t i = 0; i < g_apps.size(); ++i) if (g_apps[i].def.id == detail) g_detail = (int)i;
            g_page = page == L"settings" ? Page::Settings : (page == L"details" && g_detail >= 0) ? Page::Details : Page::Library;
            g_scroll = g_scrollTarget = (float)v.GetNum(L"scroll", 0);
            if (v.GetBool(L"minimized", false)) show = SW_SHOWMINNOACTIVE;
            else if (v.GetBool(L"background", false)) show = SW_SHOWNOACTIVATE;
        }
        if (CompareVersions(after.fromVersion, HANGAR_VERSION) < 0) {
            std::wstring notes;
            LoadResourceText(101, notes);
            g_whatsNew = CleanNotes(notes);
            if (g_whatsNew.empty()) g_whatsNew = L"See the release notes on GitHub.";
            g_whatsNewVersion = HANGAR_VERSION;
        } else {
            Toast(std::wstring(L"Rolled back to Hangar ") + HANGAR_VERSION + L".");
        }
        Log(L"started after an update from %ls", after.fromVersion.c_str());
    }
    // (a maximized window can't be shown without activating it; that only happens when you're away)
    bool maximize = g_cfg.maximized && (show == SW_SHOWNORMAL || show == SW_SHOWNOACTIVATE);
    ShowWindow(hwnd, maximize ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd);
    if (after.active) SetTimer(hwnd, TIMER_HEALTHY, 400, nullptr);

    // then refresh in the background
    jobs::FetchManifest(g_manifestText);
    CheckReleases(false);
    SetTimer(hwnd, TIMER_PROCS, 2500, nullptr);
    SetTimer(hwnd, TIMER_CHECK, 15 * 60 * 1000, nullptr);
    SetTimer(hwnd, TIMER_SELF, 20 * 1000, nullptr);   // first check about 20 s after startup, then every check interval
    return hwnd;
}

bool UiHandleAccelerator(MSG*) { return false; }
