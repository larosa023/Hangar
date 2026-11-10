// Hangar - background work
#include "jobs.h"
#include "net.h"
#include "util.h"
#include "config.h"
#include <wincodec.h>
#include <shlobj.h>
#include <shellapi.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <map>
#include <set>
#include <memory>
#include <ctime>
#include <algorithm>

namespace jobs {

static HWND s_hwnd = nullptr;
static std::atomic<bool> s_shutdown{false};
static std::atomic<long long> s_rateUntil{0};
static std::atomic<int> s_activeOps{0};
static std::mutex s_cancelLock;
static std::map<std::wstring, std::shared_ptr<std::atomic<bool>>> s_cancel;

// at most a few network requests at a time, so a big library doesn't flood the connection
class Gate {
public:
    explicit Gate(int n) : n_(n) {}
    void Enter() { std::unique_lock<std::mutex> l(m_); cv_.wait(l, [&] { return n_ > 0; }); --n_; }
    void Leave() { { std::lock_guard<std::mutex> l(m_); ++n_; } cv_.notify_one(); }
private:
    std::mutex m_;
    std::condition_variable cv_;
    int n_;
};
static Gate s_netGate(4);
struct GateHold { GateHold() { s_netGate.Enter(); } ~GateHold() { s_netGate.Leave(); } };

template <class T> static void Post(UINT msg, T* obj) {
    if (s_shutdown || !s_hwnd || !PostMessageW(s_hwnd, msg, 0, (LPARAM)obj)) delete obj;
}

template <class F> static void Run(F&& f) {
    std::thread([f = std::forward<F>(f)]() mutable {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        f();
        if (SUCCEEDED(hr)) CoUninitialize();
    }).detach();
}

void Init(HWND notify) { s_hwnd = notify; }

void Shutdown() {
    s_shutdown = true;
    std::lock_guard<std::mutex> g(s_cancelLock);
    for (auto& kv : s_cancel) *kv.second = true;
}

long long RateLimitedUntil() {
    long long t = s_rateUntil;
    return t > (long long)time(nullptr) ? t : 0;
}

int ActiveOps() { return s_activeOps; }

// ---------------------------------------------------------------- manifest
static std::wstring ManifestUrl() {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(L"HANGAR_MANIFEST_URL", buf, 1024);
    if (n > 0 && n < 1024) return buf;
    return HANGAR_MANIFEST_URL;
}

void FetchManifest(const std::wstring& currentText) {
    Run([currentText] {
        GateHold gate;
        auto* res = new ManifestResult;
        std::wstring etagPath = CacheDir() + L"\\manifest.etag", cachePath = CacheDir() + L"\\manifest.json";
        std::wstring etag;
        if (FileExists(cachePath)) ReadTextFile(etagPath, etag);
        net::Response r;
        if (!net::Get(ManifestUrl(), r, Trim(etag))) {
            res->error = r.error;
        } else if (r.status == 304) {
            // unchanged
        } else if (r.status == 200) {
            std::wstring text = Utf8ToWide(r.body);
            Manifest m;
            std::wstring err;
            if (!ParseManifest(text, m, &err)) {
                res->error = L"The online manifest has a problem: " + err;
                Log(L"manifest: %ls", res->error.c_str());
            } else {
                WriteFileAtomic(cachePath, r.body);
                WriteTextFileAtomic(etagPath, r.etag);
                if (text != currentText) { res->changed = true; res->text = text; }
            }
        } else {
            res->error = L"Couldn't get the app list (HTTP " + std::to_wstring(r.status) + L").";
        }
        if (!res->error.empty()) Log(L"manifest fetch: %ls", res->error.c_str());
        Post(WM_HANGAR_MANIFEST, res);
    });
}

// ---------------------------------------------------------------- releases
static std::wstring ReleaseCache(const std::wstring& id, const wchar_t* ext) { return CacheDir() + L"\\releases\\" + id + ext; }

static long long ReadTime(const std::wstring& id) {
    std::wstring t;
    if (!ReadTextFile(ReleaseCache(id, L".time"), t)) return 0;
    return _wtoi64(t.c_str());
}

void LoadCachedReleases(const AppDef& def) {
    Run([def] {
        std::string body;
        if (!ReadFileBytes(ReleaseCache(def.id, L".json"), body)) return;
        auto* res = new ReleasesResult;
        res->id = def.id;
        res->fromCache = true;
        res->ok = ParseReleases(body, def, res->releases, &res->error);
        res->checkedAt = ReadTime(def.id);
        Post(WM_HANGAR_RELEASES, res);
    });
}

void FetchReleases(const AppDef& def, bool force) {
    Run([def, force] {
        long long now = (long long)time(nullptr);
        long long last = ReadTime(def.id);
        std::string cached;
        bool haveCache = ReadFileBytes(ReleaseCache(def.id, L".json"), cached);
        auto* res = new ReleasesResult;
        res->id = def.id;
        if (!force && haveCache && now - last < (long long)g_cfg.checkHours * 3600) {
            res->ok = ParseReleases(cached, def, res->releases, &res->error);
            res->checkedAt = last;
            Post(WM_HANGAR_RELEASES, res);
            return;
        }
        if (RateLimitedUntil()) {
            res->error = L"GitHub's hourly limit was reached. Hangar will check again later.";
            if (haveCache) res->ok = ParseReleases(cached, def, res->releases, nullptr);
            res->checkedAt = last;
            Post(WM_HANGAR_RELEASES, res);
            return;
        }
        GateHold gate;
        std::wstring etag;
        if (haveCache) ReadTextFile(ReleaseCache(def.id, L".etag"), etag);
        net::Response r;
        std::wstring url = net::ApiBase() + L"/repos/" + def.owner + L"/" + def.repoName + L"/releases?per_page=15";
        bool sent = net::Get(url, r, Trim(etag), true);
        if (r.rateRemaining == 0 && r.rateReset > 0) s_rateUntil = r.rateReset;
        if (!sent) {
            res->error = r.error;
        } else if (r.status == 304 && haveCache) {
            WriteTextFileAtomic(ReleaseCache(def.id, L".time"), std::to_wstring(now));
            res->ok = ParseReleases(cached, def, res->releases, &res->error);
            res->checkedAt = now;
        } else if (r.status == 200) {
            res->ok = ParseReleases(r.body, def, res->releases, &res->error);
            if (res->ok) {
                WriteFileAtomic(ReleaseCache(def.id, L".json"), r.body);
                WriteTextFileAtomic(ReleaseCache(def.id, L".etag"), r.etag);
                WriteTextFileAtomic(ReleaseCache(def.id, L".time"), std::to_wstring(now));
                res->checkedAt = now;
            }
        } else if (r.status == 404) {
            res->error = L"Couldn't find " + def.repo + L" on GitHub (it may be private).";
        } else if (r.status == 403 || r.status == 429) {
            res->error = L"GitHub's hourly limit was reached. Hangar will check again later.";
            if (r.rateReset > 0) s_rateUntil = r.rateReset;
            else s_rateUntil = now + 15 * 60;
        } else {
            res->error = L"GitHub answered with HTTP " + std::to_wstring(r.status) + L".";
        }
        if (!res->ok && haveCache) {   // keep showing what we knew
            ParseReleases(cached, def, res->releases, nullptr);
            res->checkedAt = last;
        }
        if (!res->error.empty()) Log(L"releases %ls: %ls", def.id.c_str(), res->error.c_str());
        Post(WM_HANGAR_RELEASES, res);
    });
}

// ---------------------------------------------------------------- icons
static uint32_t Fnv(const std::wstring& s) {
    uint32_t h = 2166136261u;
    for (wchar_t c : s) { h ^= (uint32_t)c; h *= 16777619u; }
    return h;
}

static bool DecodeImage(const std::wstring& path, UINT px, IconResult& out) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
        // .ico files hold several sizes: take the largest frame
        UINT frames = 0, best = 0, bestW = 0;
        dec->GetFrameCount(&frames);
        for (UINT i = 0; i < frames && i < 16; ++i) {
            IWICBitmapFrameDecode* f = nullptr;
            if (SUCCEEDED(dec->GetFrame(i, &f))) {
                UINT w = 0, h = 0;
                f->GetSize(&w, &h);
                if (w > bestW) { bestW = w; best = i; }
                f->Release();
            }
        }
        if (SUCCEEDED(dec->GetFrame(best, &frame))) {
            UINT w = 0, h = 0;
            frame->GetSize(&w, &h);
            IWICBitmapSource* src = frame;
            if (w > px || h > px) {
                UINT tw = px, th = px;
                if (w > h) th = std::max<UINT>(1, h * px / w); else tw = std::max<UINT>(1, w * px / h);
                if (SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
                    SUCCEEDED(scaler->Initialize(frame, tw, th, WICBitmapInterpolationModeFant))) {
                    src = scaler;
                    w = tw; h = th;
                }
            }
            if (SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                out.w = w; out.h = h;
                out.pixels.resize((size_t)w * h * 4);
                ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)out.pixels.size(), out.pixels.data()));
            }
        }
    }
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    wic->Release();
    return ok;
}

void FetchIcon(const AppDef& def, UINT px) {
    if (def.icon.empty()) return;
    Run([def, px] {
        std::wstring ext = IContains(def.icon, L".ico") ? L".ico" : IContains(def.icon, L".jpg") ? L".jpg" : L".png";
        wchar_t hash[16];
        swprintf(hash, 16, L"%08x", Fnv(def.icon));
        std::wstring path = CacheDir() + L"\\icons\\" + def.id + L"-" + hash + ext;
        if (!FileExists(path)) {
            GateHold gate;
            std::wstring err;
            if (!net::Download(def.icon, path, nullptr, &err)) { Log(L"icon %ls: %ls", def.id.c_str(), err.c_str()); return; }
        }
        auto* res = new IconResult;
        res->id = def.id;
        if (DecodeImage(path, px, *res)) Post(WM_HANGAR_ICON, res);
        else { DeleteFileW(path.c_str()); delete res; Log(L"icon %ls: couldn't decode", def.id.c_str()); }
    });
}

// ---------------------------------------------------------------- scanning
void Scan(const std::vector<AppDef>& defs) {
    Run([defs] {
        auto* res = new ScanResult;
        for (auto& d : defs) res->items.emplace_back(d.id, DetectInstalled(d));
        Post(WM_HANGAR_SCAN, res);
    });
}

void ScanProcesses(const std::vector<std::wstring>& ids, const std::vector<std::wstring>& exes, const std::vector<std::wstring>& dirs) {
    Run([ids, exes, dirs] {
        auto* res = new ProcResult;
        res->ids = ids;
        res->pids = FindRunning(exes, dirs);
        Post(WM_HANGAR_PROCS, res);
    });
}

// ---------------------------------------------------------------- operations
struct OpContext {
    OpRequest req;
    std::shared_ptr<std::atomic<bool>> cancel;
    void Progress(int pct, const std::wstring& text) {
        auto* p = new OpProgress;
        p->id = req.def.id;
        p->percent = pct;
        p->text = text;
        Post(WM_HANGAR_OP_PROGRESS, p);
    }
};

static std::wstring ExpectedHashFromSums(const std::string& sums, const std::wstring& assetName) {
    std::wstring text = Utf8ToWide(sums);
    size_t i = 0;
    std::wstring single;
    int lines = 0;
    while (i < text.size()) {
        size_t e = text.find(L'\n', i);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = Trim(text.substr(i, e - i));
        i = e + 1;
        if (line.empty()) continue;
        ++lines;
        size_t sp = line.find_first_of(L" \t");
        std::wstring hash = Lower(line.substr(0, sp));
        if (hash.size() != 64 || hash.find_first_not_of(L"0123456789abcdef") != std::wstring::npos) continue;
        std::wstring name = sp == std::wstring::npos ? L"" : Trim(line.substr(sp));
        if (!name.empty() && name[0] == L'*') name.erase(0, 1);
        if (IEquals(name, assetName)) return hash;
        if (name.empty()) single = hash;
    }
    return lines == 1 ? single : L"";
}

// Downloads the release asset and proves it's the file the developer published.
static bool DownloadVerified(OpContext& ctx, const Asset& asset, std::wstring& outPath, std::wstring& error) {
    const ReleaseInfo& rel = ctx.req.release;
    std::wstring dir = DownloadDir() + L"\\" + ctx.req.def.id + L"\\" + rel.version;
    outPath = dir + L"\\" + asset.name;
    std::wstring expected = asset.sha256;

    // the app's own checksum file, when it publishes one
    if (!ctx.req.def.checksums.empty()) {
        if (const Asset* sums = FindAsset(rel, ctx.req.def.checksums)) {
            net::Response r;
            GateHold gate;
            if (net::Get(sums->url, r) && r.status == 200) {
                std::wstring fromFile = ExpectedHashFromSums(r.body, asset.name);
                if (!fromFile.empty()) {
                    if (!expected.empty() && fromFile != expected) {
                        error = L"The checksum file doesn't match GitHub's record of " + asset.name + L". Not installing it.";
                        return false;
                    }
                    expected = fromFile;
                }
            }
        }
    }
    if (expected.empty()) {
        error = L"This release has no checksum, so Hangar can't verify it. Not installing it.";
        return false;
    }

    // reuse an earlier, already verified download
    if (FileExists(outPath) && Sha256File(outPath) == expected) { ctx.Progress(100, L"Already downloaded"); return true; }

    ctx.Progress(0, L"Downloading…");
    int lastPct = -1;
    DWORD lastTick = 0;
    GateHold gate;
    bool ok = net::Download(asset.url, outPath, [&](uint64_t done, uint64_t total) {
        if (*ctx.cancel) return false;
        if (!total) total = asset.size;
        int pct = total ? (int)(done * 100 / total) : -1;
        DWORD now = GetTickCount();
        if (pct != lastPct && now - lastTick > 60) {
            lastPct = pct;
            lastTick = now;
            ctx.Progress(pct, total ? L"Downloading " + FormatBytes(done) + L" of " + FormatBytes(total) : L"Downloading " + FormatBytes(done));
        }
        return true;
    }, &error);
    if (!ok) return false;
    ctx.Progress(-1, L"Verifying…");
    std::wstring actual = Sha256File(outPath);
    if (actual != expected) {
        DeleteFileW(outPath.c_str());
        error = L"The download didn't match its checksum, so Hangar deleted it. Try again.";
        Log(L"%ls: sha256 mismatch: expected %ls got %ls", ctx.req.def.id.c_str(), expected.c_str(), actual.c_str());
        return false;
    }
    Log(L"%ls: downloaded and verified %ls (sha256 %ls)", ctx.req.def.id.c_str(), asset.name.c_str(), actual.c_str());
    return true;
}

struct CloseWindowsCtx { std::set<DWORD> pids; int posted = 0; };

static BOOL CALLBACK CloseEnum(HWND hwnd, LPARAM lp) {
    auto* c = (CloseWindowsCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (c->pids.count(pid) && !GetWindow(hwnd, GW_OWNER)) { PostMessageW(hwnd, WM_CLOSE, 0, 0); ++c->posted; }
    return TRUE;
}

static std::vector<DWORD> RunningPids(const Installed& in) {
    if (in.exe.empty()) return {};
    auto r = FindRunning({in.exe}, {in.dir});
    return r.empty() ? std::vector<DWORD>{} : r[0];
}

static bool WaitForExit(const std::vector<DWORD>& pids, DWORD ms) {
    std::vector<HANDLE> hs;
    for (DWORD pid : pids) if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) hs.push_back(h);
    bool ok = true;
    DWORD start = GetTickCount();
    for (HANDLE h : hs) {
        DWORD left = ms - std::min<DWORD>(ms, GetTickCount() - start);
        if (WaitForSingleObject(h, left) != WAIT_OBJECT_0) ok = false;
        CloseHandle(h);
    }
    return ok;
}

// Asks the app to close the way a person would, never by force.
static bool CloseApp(OpContext& ctx, std::wstring& error) {
    std::vector<DWORD> pids = RunningPids(ctx.req.inst);
    if (pids.empty()) return true;
    ctx.Progress(-1, L"Closing " + ctx.req.def.name + L"…");
    if (!ctx.req.def.quitArg.empty()) {
        DWORD code;
        RunAndWait(ctx.req.inst.exe, ctx.req.def.quitArg, 5000, &code, true);
    } else {
        CloseWindowsCtx c;
        c.pids.insert(pids.begin(), pids.end());
        EnumWindows(CloseEnum, (LPARAM)&c);
    }
    if (WaitForExit(pids, 10000)) { Sleep(300); return true; }
    error = ctx.req.def.name + L" didn't close. Save your work, close it, and try again.";
    return false;
}

static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& desc) {
    IShellLinkW* link = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
        link->SetPath(target.c_str());
        link->SetWorkingDirectory(PathParent(target).c_str());
        link->SetDescription(desc.c_str());
        link->SetIconLocation(target.c_str(), 0);
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&pf)))) ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
    }
    if (pf) pf->Release();
    if (link) link->Release();
    return ok;
}

static void SetRegStr(HKEY key, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

// Registers a single-exe app the same way the app registers itself (Start menu + Apps & features).
static void RegisterPortable(const AppDef& def, const std::wstring& exe, const std::wstring& version) {
    std::wstring lnk = KnownFolder(FOLDERID_Programs) + L"\\" + def.name + L".lnk";
    if (!FileExists(lnk)) MakeShortcut(lnk, exe, def.name + L" - " + def.description);
    if (def.uninstallArg.empty()) return;   // Apps & features would have no way to remove it
    std::wstring key = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + def.name;
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    SetRegStr(h, L"DisplayName", def.name);
    SetRegStr(h, L"DisplayVersion", version);
    SetRegStr(h, L"Publisher", def.owner);
    SetRegStr(h, L"DisplayIcon", exe + L",0");
    SetRegStr(h, L"InstallLocation", PathParent(exe));
    SetRegStr(h, L"UninstallString", L"\"" + exe + L"\" " + def.uninstallArg);
    SetRegStr(h, L"URLInfoAbout", L"https://github.com/" + def.repo);
    DWORD one = 1;
    RegSetValueExW(h, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegSetValueExW(h, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegCloseKey(h);
}

// Starts the new exe and watches it for a few seconds. False if it quit with an error.
static bool StartsHealthy(const std::wstring& exe) {
    std::wstring cmd = L"\"" + exe + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, PathParent(exe).c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread);
    DWORD w = WaitForSingleObject(pi.hProcess, 4000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    // exiting with 0 is fine: single-instance apps hand off to a copy that's already running
    return w == WAIT_TIMEOUT || code == 0;
}

static bool InstallPortable(OpContext& ctx, const std::wstring& file, bool wasRunning, std::wstring& error) {
    const AppDef& def = ctx.req.def;
    std::wstring dir = ctx.req.inst.installed && !ctx.req.inst.dir.empty() ? ctx.req.inst.dir : PortableDir(def);
    std::wstring target = dir + L"\\" + def.exe;
    EnsureDir(dir);
    if (!DirExists(dir)) { error = L"Couldn't create " + dir + L"."; return false; }
    ctx.Progress(-1, L"Installing…");
    std::wstring old = target + L".old";
    bool hadOld = FileExists(target);
    DeleteFileW(old.c_str());
    if (hadOld && !MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = L"Couldn't replace " + def.exe + L" (is it still running?).";
        return false;
    }
    if (!CopyFileW(file.c_str(), target.c_str(), FALSE)) {
        DWORD e = GetLastError();
        if (hadOld) MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
        error = L"Couldn't copy " + def.exe + L" into " + dir + L" (error " + std::to_wstring(e) + L").";
        return false;
    }
    RegisterPortable(def, target, ctx.req.release.version);
    if (wasRunning) {
        ctx.Progress(-1, L"Starting " + def.name + L"…");
        if (!StartsHealthy(target)) {
            Log(L"%ls %ls failed to start; rolling back", def.id.c_str(), ctx.req.release.version.c_str());
            if (hadOld) {
                MoveFileExW(old.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
                LaunchDetached(target, L"");
                error = L"Version " + ctx.req.release.version + L" didn't start, so Hangar put the previous version back.";
            } else {
                error = def.name + L" didn't start.";
            }
            return false;
        }
    }
    if (hadOld && !DeleteFileW(old.c_str())) MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
}

static bool RunSetup(OpContext& ctx, const std::wstring& file, std::wstring& error) {
    ctx.Progress(-1, L"Installing…");
    DWORD code = 0;
    if (!RunAndWait(file, ctx.req.def.silentArgs, 10 * 60 * 1000, &code, false)) {
        error = L"The installer didn't finish.";
        return false;
    }
    if (code != 0) {
        error = L"The installer reported an error (code " + std::to_wstring(code) + L").";
        return false;
    }
    return true;
}

static void SplitCommand(const std::wstring& cmd0, std::wstring& exe, std::wstring& args) {
    std::wstring cmd = Trim(cmd0);
    if (!cmd.empty() && cmd[0] == L'"') {
        size_t e = cmd.find(L'"', 1);
        exe = cmd.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1);
        args = e == std::wstring::npos ? L"" : Trim(cmd.substr(e + 1));
    } else {
        size_t p = Lower(cmd).find(L".exe");
        if (p == std::wstring::npos) { exe = cmd; args.clear(); return; }
        exe = cmd.substr(0, p + 4);
        args = Trim(cmd.substr(p + 4));
    }
}

static bool Uninstall(OpContext& ctx, std::wstring& error) {
    const AppDef& def = ctx.req.def;
    const Installed& in = ctx.req.inst;
    if (def.type == InstallType::Portable && !def.uninstallArg.empty() && FileExists(in.exe)) {
        // the app knows how to remove itself (and undo things like "default file manager")
        ctx.Progress(-1, L"Waiting for " + def.name + L"'s uninstaller…");
        DWORD code = 0;
        RunAndWait(in.exe, def.uninstallArg, 10 * 60 * 1000, &code, false);
    } else if (def.type == InstallType::Portable) {
        if (!CloseApp(ctx, error)) return false;
        ctx.Progress(-1, L"Removing…");
        DeleteFileW((in.exe + L".old").c_str());
        if (!DeleteFileW(in.exe.c_str()) && FileExists(in.exe)) { error = L"Couldn't delete " + in.exe + L"."; return false; }
        RemoveDirectoryW(in.dir.c_str());   // only if empty
        DeleteFileW((KnownFolder(FOLDERID_Programs) + L"\\" + def.name + L".lnk").c_str());
        HKEY run;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &run) == ERROR_SUCCESS) {
            RegDeleteValueW(run, def.name.c_str());
            RegCloseKey(run);
        }
        if (!in.regKey.empty() && !in.machineWide) RegDeleteTreeW(HKEY_CURRENT_USER, in.regKey.c_str());
        return true;
    } else {
        if (!CloseApp(ctx, error)) return false;
        std::wstring cmd = in.quietUninstallCmd;
        if (cmd.empty()) {
            cmd = in.uninstallCmd;
            if (def.type == InstallType::Nsis && !cmd.empty() && !IContains(cmd, L"/S")) cmd += L" /S";
        }
        if (cmd.empty()) { error = L"Windows has no uninstaller registered for " + def.name + L"."; return false; }
        std::wstring exe, args;
        SplitCommand(cmd, exe, args);
        ctx.Progress(-1, L"Uninstalling…");
        DWORD code = 0;
        if (!RunAndWait(exe, args, 10 * 60 * 1000, &code, false)) { error = L"The uninstaller didn't finish."; return false; }
    }
    // NSIS uninstallers copy themselves to %TEMP% and return at once; wait until the app is really gone
    for (int i = 0; i < 60; ++i) {
        if (!DetectInstalled(def).installed) return true;
        Sleep(1000);
    }
    error = def.name + L" is still installed (the uninstaller may have been cancelled).";
    return false;
}

static void DoOp(OpContext ctx) {
    auto* done = new OpDone;
    done->id = ctx.req.def.id;
    done->kind = ctx.req.kind;
    std::wstring error;
    const AppDef& def = ctx.req.def;
    Log(L"%ls: %ls starting", def.id.c_str(),
        ctx.req.kind == OpKind::Install ? L"install" : ctx.req.kind == OpKind::Update ? L"update" : ctx.req.kind == OpKind::Repair ? L"repair" : L"uninstall");

    if (ctx.req.kind == OpKind::Uninstall) {
        done->ok = Uninstall(ctx, error);
        done->message = done->ok ? def.name + L" was uninstalled." : error;
    } else {
        const Asset* asset = FindInstallAsset(ctx.req.release, def, ctx.req.beta && ctx.req.release.prerelease);
        std::wstring file;
        if (!asset) error = L"Release " + ctx.req.release.version + L" doesn't include " + def.asset + L".";
        else if (DownloadVerified(ctx, *asset, file, error)) {
            bool wasRunning = !RunningPids(ctx.req.inst).empty();
            if (CloseApp(ctx, error)) {
                bool ok = def.type == InstallType::Portable ? InstallPortable(ctx, file, wasRunning, error) : RunSetup(ctx, file, error);
                if (ok && def.type != InstallType::Portable) {
                    // setups don't start the app when silent; reopen it if it was open
                    Installed now = DetectInstalled(def);
                    if (wasRunning && now.installed) LaunchDetached(now.exe, L"");
                    if (!now.installed) { ok = false; error = L"The installer finished, but " + def.name + L" isn't showing as installed."; }
                }
                if (ok) {
                    done->ok = true;
                    done->version = ctx.req.release.version;
                    done->message = def.name + (ctx.req.kind == OpKind::Install ? L" " + ctx.req.release.version + L" is installed."
                                              : ctx.req.kind == OpKind::Update ? L" updated to " + ctx.req.release.version + L"."
                                              : L" was repaired.");
                    // the verified download isn't needed any more
                    DeleteFileW(file.c_str());
                    RemoveDirectoryW(PathParent(file).c_str());
                }
            }
        }
        if (!done->ok) done->message = error;
    }
    done->cancelled = *ctx.cancel;
    if (done->cancelled) done->message = L"Cancelled.";
    Log(L"%ls: %ls", def.id.c_str(), done->message.c_str());
    {
        std::lock_guard<std::mutex> g(s_cancelLock);
        s_cancel.erase(def.id);
    }
    --s_activeOps;
    Post(WM_HANGAR_OP_DONE, done);
}

void StartOp(const OpRequest& req) {
    OpContext ctx;
    ctx.req = req;
    ctx.cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> g(s_cancelLock);
        s_cancel[req.def.id] = ctx.cancel;
    }
    ++s_activeOps;
    Run([ctx] { DoOp(ctx); });
}

void CancelOp(const std::wstring& id) {
    std::lock_guard<std::mutex> g(s_cancelLock);
    auto it = s_cancel.find(id);
    if (it != s_cancel.end()) *it->second = true;
}

} // namespace jobs
