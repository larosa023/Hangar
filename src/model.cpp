// Hangar - the app catalog: manifest, releases, install detection, running processes
#include "model.h"
#include "util.h"
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>

// ---------------------------------------------------------------- manifest
std::wstring InstallTypeName(InstallType t) {
    switch (t) {
        case InstallType::Nsis: return L"Setup (NSIS)";
        case InstallType::Velopack: return L"Velopack";
        default: return L"Single exe";
    }
}

static bool ParseApp(const json::Value& a, AppDef& d, std::wstring* error) {
    d.id = Lower(Trim(a.GetStr(L"id")));
    d.name = a.GetStr(L"name");
    d.repo = a.GetStr(L"repo");
    if (d.id.empty() || d.name.empty() || d.repo.empty()) { if (error) *error = L"An app is missing its id, name or repo."; return false; }
    size_t slash = d.repo.find(L'/');
    if (slash == std::wstring::npos) { if (error) *error = d.name + L": repo must look like owner/name."; return false; }
    d.owner = d.repo.substr(0, slash);
    d.repoName = d.repo.substr(slash + 1);
    d.description = a.GetStr(L"description");
    d.category = Lower(a.GetStr(L"category", L"other"));
    d.icon = a.GetStr(L"icon");
    d.exe = a.GetStr(L"exe", d.name + L".exe");
    d.tagPrefix = a.GetStr(L"tagPrefix", L"v");
    d.minHangar = a.GetStr(L"minHangar");
    d.critical = a.GetBool(L"critical", false);
    d.liveUpdate = a.GetBool(L"liveUpdate", false);
    if (const json::Value* s = a.Get(L"screenshots"); s && s->type == json::Value::Array)
        for (auto& v : s->arr) if (v.type == json::Value::String) d.screenshots.push_back(v.s);
    const json::Value* in = a.Get(L"install");
    if (!in || in->type != json::Value::Object) { if (error) *error = d.name + L": missing \"install\"."; return false; }
    std::wstring type = Lower(in->GetStr(L"type", L"portable"));
    d.type = type == L"nsis" ? InstallType::Nsis : type == L"velopack" ? InstallType::Velopack : InstallType::Portable;
    d.asset = in->GetStr(L"asset");
    d.betaAsset = in->GetStr(L"betaAsset");
    d.checksums = in->GetStr(L"checksums");
    d.registry = in->GetStr(L"registry", d.name);
    d.packId = in->GetStr(L"packId", d.name);
    d.dir = in->GetStr(L"dir", L"%LOCALAPPDATA%\\Programs\\" + d.name);
    d.silentArgs = in->GetStr(L"silentArgs", d.type == InstallType::Nsis ? L"/S" : d.type == InstallType::Velopack ? L"--silent" : L"");
    if (d.asset.empty()) {
        if (d.type == InstallType::Portable) d.asset = d.exe;
        else if (d.type == InstallType::Velopack) d.asset = d.packId + L"-win-Setup.exe";
        else { if (error) *error = d.name + L": an NSIS app needs install.asset."; return false; }
    }
    if (const json::Value* args = a.Get(L"args"); args && args->type == json::Value::Object) {
        d.quitArg = args->GetStr(L"quit");
        d.uninstallArg = args->GetStr(L"uninstall");
    }
    return true;
}

bool ParseManifest(const std::wstring& text, Manifest& m, std::wstring* error, bool local) {
    json::Value root;
    if (!json::Parser(text).Parse(root) || root.type != json::Value::Object) { if (error) *error = L"The manifest isn't valid JSON."; return false; }
    int schema = (int)root.GetNum(L"schema", 1);
    if (schema > 1) { if (error) *error = L"This manifest needs a newer Hangar."; return false; }
    m = Manifest{};
    if (const json::Value* c = root.Get(L"categories"); c && c->type == json::Value::Array)
        for (auto& v : c->arr) {
            Category cat{Lower(v.GetStr(L"id")), v.GetStr(L"name")};
            if (!cat.id.empty()) m.categories.push_back(cat);
        }
    if (const json::Value* apps = root.Get(L"apps"); apps && apps->type == json::Value::Array) {
        for (auto& v : apps->arr) {
            AppDef d;
            std::wstring err;
            if (!ParseApp(v, d, &err)) { Log(L"manifest: skipping an app: %ls", err.c_str()); continue; }
            d.local = local;
            bool dup = false;
            for (auto& x : m.apps) if (x.id == d.id) dup = true;
            if (!dup) m.apps.push_back(std::move(d));
        }
    }
    if (m.apps.empty() && !local) { if (error) *error = L"The manifest lists no apps."; return false; }
    return true;
}

void MergeManifest(Manifest& base, const Manifest& o) {
    for (auto& c : o.categories) {
        bool found = false;
        for (auto& b : base.categories) if (b.id == c.id) { b.name = c.name; found = true; }
        if (!found) base.categories.push_back(c);
    }
    for (auto& a : o.apps) {
        bool found = false;
        for (auto& b : base.apps) if (b.id == a.id) { b = a; found = true; }
        if (!found) base.apps.push_back(a);
    }
}

// ---------------------------------------------------------------- releases
bool ParseReleases(const std::string& body, const AppDef& def, std::vector<ReleaseInfo>& out, std::wstring* error) {
    json::Value root;
    if (!json::Parser(Utf8ToWide(body)).Parse(root) || root.type != json::Value::Array) {
        if (error) *error = L"GitHub sent an unexpected reply.";
        return false;
    }
    out.clear();
    for (const json::Value& r : root.arr) {
        if (r.GetBool(L"draft", false)) continue;
        ReleaseInfo info;
        info.tag = r.GetStr(L"tag_name");
        std::wstring v = info.tag;
        if (!def.tagPrefix.empty() && IStartsWith(v, def.tagPrefix)) v = v.substr(def.tagPrefix.size());
        else if (!def.tagPrefix.empty() && def.tagPrefix != L"v") continue;     // e.g. Fullcourt's "desktop-v" releases only
        v = NormalizeVersion(v);
        if (v.empty() || !iswdigit(v[0])) continue;
        info.version = v;
        info.title = r.GetStr(L"name", info.tag);
        info.notes = r.GetStr(L"body");
        info.published = r.GetStr(L"published_at");
        info.page = r.GetStr(L"html_url");
        info.prerelease = r.GetBool(L"prerelease", false) || v.find(L'-') != std::wstring::npos;
        if (const json::Value* assets = r.Get(L"assets"); assets && assets->type == json::Value::Array) {
            for (const json::Value& a : assets->arr) {
                Asset as;
                as.name = a.GetStr(L"name");
                as.url = a.GetStr(L"browser_download_url");
                as.size = (uint64_t)a.GetNum(L"size", 0);
                std::wstring digest = a.GetStr(L"digest");
                if (IStartsWith(digest, L"sha256:")) as.sha256 = Lower(digest.substr(7));
                if (!as.name.empty() && !as.url.empty()) info.assets.push_back(as);
            }
        }
        out.push_back(std::move(info));
    }
    std::stable_sort(out.begin(), out.end(), [](const ReleaseInfo& a, const ReleaseInfo& b) { return CompareVersions(a.version, b.version) > 0; });
    return true;
}

const Asset* FindAsset(const ReleaseInfo& r, const std::wstring& name) {
    for (auto& a : r.assets) if (IEquals(a.name, name)) return &a;
    return nullptr;
}

const Asset* FindInstallAsset(const ReleaseInfo& r, const AppDef& def, bool beta) {
    std::vector<std::wstring> patterns;
    if (beta && !def.betaAsset.empty()) patterns.push_back(def.betaAsset);
    patterns.push_back(def.asset);
    for (auto& p : patterns)
        for (auto& a : r.assets)
            if (WildMatch(p, a.name) && !IContains(a.name, L".blockmap") && !IContains(a.name, L".sig")) return &a;
    return nullptr;
}

const ReleaseInfo* LatestRelease(const std::vector<ReleaseInfo>& list, const AppDef& def, bool beta) {
    for (auto& r : list) {
        if (r.prerelease && !beta) continue;
        if (FindInstallAsset(r, def, r.prerelease && beta)) return &r;
    }
    return nullptr;
}

// ---------------------------------------------------------------- detection
static std::wstring RegStr(HKEY key, const wchar_t* name) {
    wchar_t buf[2048];
    DWORD size = sizeof(buf), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, (BYTE*)buf, &size) != ERROR_SUCCESS) return L"";
    if (type != REG_SZ && type != REG_EXPAND_SZ) return L"";
    size_t n = size / sizeof(wchar_t);
    while (n && buf[n - 1] == 0) --n;
    std::wstring s(buf, n);
    return type == REG_EXPAND_SZ ? ExpandEnv(s) : s;
}

static std::wstring ExeFromIcon(std::wstring icon) {
    icon = Trim(icon);
    if (!icon.empty() && icon[0] == L'"') {
        size_t e = icon.find(L'"', 1);
        icon = icon.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1);
    } else {
        size_t comma = icon.rfind(L',');
        if (comma != std::wstring::npos && comma > 2) icon = icon.substr(0, comma);
    }
    return IContains(icon, L".exe") ? icon : L"";
}

struct UninstallEntry {
    std::wstring keyPath, displayName, version, location, icon, uninstall, quiet;
    bool machine = false;
};

static void ReadEntry(HKEY root, REGSAM view, const std::wstring& path, bool machine, std::vector<UninstallEntry>& out) {
    HKEY k;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | view, &k) != ERROR_SUCCESS) return;
    UninstallEntry e;
    e.keyPath = path;
    e.machine = machine;
    e.displayName = RegStr(k, L"DisplayName");
    e.version = RegStr(k, L"DisplayVersion");
    e.location = RegStr(k, L"InstallLocation");
    e.icon = RegStr(k, L"DisplayIcon");
    e.uninstall = RegStr(k, L"UninstallString");
    e.quiet = RegStr(k, L"QuietUninstallString");
    RegCloseKey(k);
    if (!e.displayName.empty()) out.push_back(e);
}

static const wchar_t* kUninstallRoot = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

// All Apps & features entries whose display name is `name` or starts with "name " (electron-builder
// appends the version). Also the entry stored under the key `name` itself.
static std::vector<UninstallEntry> FindUninstallEntries(const std::wstring& name) {
    std::vector<UninstallEntry> out;
    struct Root { HKEY h; REGSAM view; bool machine; } roots[] = {
        {HKEY_CURRENT_USER, 0, false},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, true},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, true},
    };
    for (auto& r : roots) {
        HKEY k;
        if (RegOpenKeyExW(r.h, kUninstallRoot, 0, KEY_READ | r.view, &k) != ERROR_SUCCESS) continue;
        wchar_t sub[512];
        for (DWORD i = 0;; ++i) {
            DWORD len = 512;
            if (RegEnumKeyExW(k, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::vector<UninstallEntry> one;
            std::wstring path = std::wstring(kUninstallRoot) + L"\\" + sub;
            ReadEntry(r.h, r.view, path, r.machine, one);
            for (auto& e : one) {
                bool match = IEquals(e.displayName, name) || IEquals(sub, name) ||
                             (IStartsWith(e.displayName, name + L" ") && e.displayName.size() > name.size() + 1 &&
                              (iswdigit(e.displayName[name.size() + 1]) || e.displayName[name.size() + 1] == L'v'));
                if (match) out.push_back(e);
            }
        }
        RegCloseKey(k);
    }
    return out;
}

std::wstring PortableDir(const AppDef& def) { return ExpandEnv(def.dir); }

Installed DetectInstalled(const AppDef& def) {
    Installed in;
    if (def.type == InstallType::Portable) {
        std::wstring dir = PortableDir(def);
        std::wstring exe = dir + L"\\" + def.exe;
        if (FileExists(exe)) {
            in.installed = true;
            in.exe = exe;
            in.dir = dir;
            in.version = ExeVersion(exe);
        }
        // the app may have registered itself in Apps & features
        auto entries = FindUninstallEntries(def.name);
        for (auto& e : entries) {
            std::wstring ex = ExeFromIcon(e.icon);
            if (ex.empty() && !e.location.empty()) ex = e.location + L"\\" + def.exe;
            if (!in.installed && !ex.empty() && FileExists(ex)) {
                in.installed = true;
                in.exe = ex;
                in.dir = PathParent(ex);
                in.version = ExeVersion(ex);
            }
            if (in.installed && IEquals(PathParent(ex), in.dir)) {
                if (in.version.empty()) in.version = NormalizeVersion(e.version);
                in.uninstallCmd = e.uninstall;
                in.quietUninstallCmd = e.quiet;
                in.regKey = e.keyPath;
                in.machineWide = e.machine;
                break;
            }
        }
        return in;
    }

    std::wstring name = def.type == InstallType::Velopack ? def.packId : def.registry;
    auto entries = FindUninstallEntries(name);
    if (entries.empty() && def.type == InstallType::Velopack && !IEquals(def.packId, def.name)) entries = FindUninstallEntries(def.name);
    for (auto& e : entries) {
        std::wstring exe;
        if (def.type == InstallType::Velopack) {
            std::wstring root = e.location.empty() ? KnownFolder(FOLDERID_LocalAppData) + L"\\" + def.packId : e.location;
            if (FileExists(root + L"\\current\\" + def.exe)) exe = root + L"\\current\\" + def.exe;
        }
        if (exe.empty()) exe = ExeFromIcon(e.icon);
        if ((exe.empty() || !FileExists(exe)) && !e.location.empty()) {
            std::wstring loc = e.location;
            if (!loc.empty() && loc.front() == L'"') loc = loc.substr(1, loc.size() - 2);
            if (FileExists(loc + L"\\" + def.exe)) exe = loc + L"\\" + def.exe;
        }
        if (exe.empty() || !FileExists(exe)) continue;
        in.installed = true;
        in.exe = exe;
        in.dir = PathParent(exe);
        if (def.type == InstallType::Velopack && IEquals(PathName(in.dir), L"current")) in.dir = PathParent(in.dir);
        in.version = NormalizeVersion(e.version);
        if (in.version.empty()) in.version = ExeVersion(exe);
        in.uninstallCmd = e.uninstall;
        in.quietUninstallCmd = e.quiet;
        in.regKey = e.keyPath;
        in.machineWide = e.machine;
        break;
    }
    return in;
}

// ---------------------------------------------------------------- running processes
std::vector<std::vector<DWORD>> FindRunning(const std::vector<std::wstring>& exePaths, const std::vector<std::wstring>& dirs) {
    std::vector<std::vector<DWORD>> result(exePaths.size());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        // cheap name filter first; only open processes whose name matches one of ours
        std::vector<size_t> candidates;
        for (size_t i = 0; i < exePaths.size(); ++i)
            if (!exePaths[i].empty() && IEquals(PathName(exePaths[i]), pe.szExeFile)) candidates.push_back(i);
        if (candidates.empty()) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD len = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
            std::wstring p(path, len);
            for (size_t i : candidates)
                if (IEquals(p, exePaths[i]) || (i < dirs.size() && !dirs[i].empty() && PathStartsWith(p, dirs[i])))
                    result[i].push_back(pe.th32ProcessID);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return result;
}
