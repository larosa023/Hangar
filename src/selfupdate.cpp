// Hangar - Hangar's own updater (see selfupdate.h)
#include "selfupdate.h"
#include "model.h"
#include "jobs.h"
#include "net.h"
#include "util.h"
#include "config.h"
#include <thread>
#include <atomic>
#include <shellapi.h>
#include <algorithm>

namespace selfupdate {

static std::atomic<bool> s_busy{false};
static HWND s_notify = nullptr;

static AppDef HangarDef() {
    AppDef d;
    d.id = L"hangar";
    d.name = L"Hangar";
    d.owner = HANGAR_REPO_OWNER;
    d.repoName = HANGAR_REPO_NAME;
    d.repo = d.owner + L"/" + d.repoName;
    d.exe = L"Hangar.exe";
    d.asset = L"Hangar.exe";
    d.checksums = L"SHA256SUMS.txt";
    return d;
}

static void Post(HWND hwnd, SelfUpdateState* s) {
    if (!hwnd || !PostMessageW(hwnd, WM_HANGAR_SELF, 0, (LPARAM)s)) delete s;
}

static void PostState(SelfUpdateState::Kind k, const std::wstring& version = L"", const std::wstring& msg = L"", int pct = -1) {
    auto* s = new SelfUpdateState;
    s->kind = k;
    s->version = version;
    s->message = msg;
    s->percent = pct;
    Post(s_notify, s);
}

void Check(HWND notify, bool force) {
    if (s_busy.exchange(true)) return;
    s_notify = notify;
    bool beta = g_cfg.hangarBeta;
    std::thread([beta, force] {
        PostState(SelfUpdateState::Checking);
        AppDef def = HangarDef();
        net::Response r;
        std::wstring url = net::ApiBase() + L"/repos/" + def.repo + L"/releases?per_page=10";
        std::vector<ReleaseInfo> rels;
        std::wstring err;
        if (!net::Get(url, r, L"", true)) err = r.error;
        else if (r.status != 200) err = r.status == 403 || r.status == 429 ? L"GitHub's hourly limit was reached; Hangar will try again later."
                                                                            : L"GitHub answered with HTTP " + std::to_wstring(r.status) + L".";
        else ParseReleases(r.body, def, rels, &err);
        if (!err.empty()) { Log(L"self-update check: %ls", err.c_str()); PostState(SelfUpdateState::Error, L"", err); s_busy = false; return; }

        const ReleaseInfo* best = nullptr;
        for (auto& rel : rels) {
            if (rel.prerelease && !beta) continue;
            if (!force && IsSkippedVersion(rel.version)) continue;   // failed to start, or you rolled back from it
            if (!FindAsset(rel, def.asset)) continue;
            best = &rel;
            break;
        }
        if (!best || CompareVersions(best->version, HANGAR_VERSION) <= 0) {
            PostState(SelfUpdateState::UpToDate);
            s_busy = false;
            return;
        }
        const ReleaseInfo rel = *best;
        const Asset* asset = FindAsset(rel, def.asset);
        Log(L"self-update: %ls available", rel.version.c_str());

        // expected hash: GitHub's digest, cross-checked with SHA256SUMS.txt
        std::wstring expected = asset->sha256;
        if (const Asset* sums = FindAsset(rel, def.checksums)) {
            net::Response sr;
            if (net::Get(sums->url, sr) && sr.status == 200) {
                std::wstring h = jobs::ExpectedHashFromSums(sr.body, asset->name);
                if (!h.empty() && !expected.empty() && h != expected) {
                    PostState(SelfUpdateState::Error, rel.version, L"The checksums for Hangar " + rel.version + L" don't agree. Not installing it.");
                    s_busy = false;
                    return;
                }
                if (!h.empty()) expected = h;
            }
        }
        if (expected.empty()) {
            PostState(SelfUpdateState::Error, rel.version, L"Hangar " + rel.version + L" has no checksum, so it can't be verified.");
            s_busy = false;
            return;
        }

        std::wstring path = DataDir() + L"\\updates\\" + rel.version + L"\\Hangar.exe";
        bool ok = FileExists(path) && Sha256File(path) == expected;
        if (!ok) {
            int last = -2;
            ok = net::Download(asset->url, path, [&](uint64_t done, uint64_t total) {
                if (!total) total = asset->size;
                int pct = total ? (int)(done * 100 / total) : -1;
                if (pct != last) { last = pct; PostState(SelfUpdateState::Downloading, rel.version, L"", pct); }
                return true;
            }, &err);
            if (ok && Sha256File(path) != expected) {
                DeleteFileW(path.c_str());
                ok = false;
                err = L"The download didn't match its checksum.";
            }
        }
        if (!ok) {
            Log(L"self-update download: %ls", err.c_str());
            PostState(SelfUpdateState::Error, rel.version, err);
            s_busy = false;
            return;
        }
        // tidy older staged updates
        WIN32_FIND_DATAW fd;
        std::wstring updates = DataDir() + L"\\updates";
        HANDLE f = FindFirstFileW((updates + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L".." || n == rel.version || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                DeleteFileW((updates + L"\\" + n + L"\\Hangar.exe").c_str());
                RemoveDirectoryW((updates + L"\\" + n).c_str());
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        Log(L"self-update: %ls downloaded and verified", rel.version.c_str());
        auto* s = new SelfUpdateState;
        s->kind = SelfUpdateState::Ready;
        s->version = rel.version;
        s->notes = rel.notes;
        s->staged = path;
        Post(s_notify, s);
        s_busy = false;
    }).detach();
}

bool Apply(HWND hwnd, const std::wstring& newExe, const std::wstring& version, std::wstring* error) {
    std::wstring exe = ExePath();
    std::wstring old = exe + L".old";
    // 1. swap the files (Windows lets a running exe be renamed, not overwritten)
    DeleteFileW(old.c_str());
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = L"Hangar can't update itself in " + ExeDir() + L" (no permission). Move Hangar.exe to a folder you own, like your Documents.";
        return false;
    }
    if (!CopyFileW(newExe.c_str(), exe.c_str(), FALSE)) {
        MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
        *error = L"Couldn't put the new Hangar.exe in place.";
        return false;
    }
    // 2. hand over: hide, release the single-instance lock, start the new copy
    wchar_t evName[96];
    swprintf(evName, 96, L"Local\\Hangar.larosa023.Healthy.%lu", GetCurrentProcessId());
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, evName);
    ShowWindow(hwnd, SW_HIDE);
    ReleaseInstanceMutex();
    wchar_t args[256];
    swprintf(args, 256, L"--after-update %ls --healthy-event %ls --old-pid %lu", HANGAR_VERSION, evName, GetCurrentProcessId());
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    bool started = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, ExeDir().c_str(), &si, &pi) != 0;
    bool healthy = false;
    if (started) {
        CloseHandle(pi.hThread);
        HANDLE waits[2] = {ev, pi.hProcess};
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 20000);
        healthy = w == WAIT_OBJECT_0;
        if (!healthy) {
            Log(L"self-update: Hangar %ls didn't start (wait result %lu); rolling back", version.c_str(), w);
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
        }
        CloseHandle(pi.hProcess);
    }
    if (ev) CloseHandle(ev);
    if (healthy) {
        Log(L"self-update: handed over to Hangar %ls", version.c_str());
        return true;
    }
    // 3. roll back: put ourselves back and carry on
    DeleteFileW(exe.c_str());
    MoveFileExW(old.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING);
    ReacquireInstanceMutex();
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    *error = L"Hangar " + version + L" didn't start, so you're still on " + HANGAR_VERSION + L". Hangar will skip that version.";
    return false;
}

AfterUpdate ParseArgs(const wchar_t* cmdLine) {
    AfterUpdate a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine && *cmdLine ? cmdLine : L"x", &argc);
    if (!argv) return a;
    for (int i = 0; i + 1 < argc; ++i) {
        std::wstring k = argv[i];
        if (k == L"--after-update") { a.active = true; a.fromVersion = argv[i + 1]; }
        else if (k == L"--healthy-event") a.healthyEvent = argv[i + 1];
        else if (k == L"--old-pid") a.oldPid = (DWORD)_wtoi(argv[i + 1]);
    }
    LocalFree(argv);
    return a;
}

void SignalHealthy(const AfterUpdate& a) {
    if (!a.active || a.healthyEvent.empty()) return;
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, a.healthyEvent.c_str())) {
        SetEvent(ev);
        CloseHandle(ev);
    }
}

static std::wstring VersionsDir() { return DataDir() + L"\\versions"; }

void FinishAfterUpdate(const AfterUpdate& a) {
    if (!a.active) return;
    AfterUpdate copy = a;
    std::thread([copy] {
        // wait for the old copy to exit, then keep it for manual rollback
        if (copy.oldPid) {
            if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, copy.oldPid)) { WaitForSingleObject(h, 15000); CloseHandle(h); }
        }
        std::wstring old = ExePath() + L".old";
        if (!FileExists(old)) return;
        std::wstring ver = NormalizeVersion(copy.fromVersion);
        bool kept = false;
        if (!ver.empty() && ver.find_first_of(L"\\/:") == std::wstring::npos) {
            std::wstring dir = VersionsDir() + L"\\" + ver;
            EnsureDir(dir);
            kept = CopyFileW(old.c_str(), (dir + L"\\Hangar.exe").c_str(), FALSE) != 0;
        }
        for (int i = 0; i < 10 && FileExists(old); ++i) { if (DeleteFileW(old.c_str())) break; Sleep(1000); }
        if (FileExists(old)) MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        Log(L"after update from %ls: previous version %ls", ver.c_str(), kept ? L"kept for rollback" : L"removed");
        // keep the newest 2 previous versions
        auto prev = PreviousVersions();
        for (size_t i = 2; i < prev.size(); ++i) {
            DeleteFileW(prev[i].exe.c_str());
            RemoveDirectoryW(PathParent(prev[i].exe).c_str());
        }
    }).detach();
}

std::vector<Previous> PreviousVersions() {
    std::vector<Previous> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((VersionsDir() + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring exe = VersionsDir() + L"\\" + n + L"\\Hangar.exe";
        if (FileExists(exe) && CompareVersions(n, HANGAR_VERSION) != 0) out.push_back({n, exe});
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const Previous& a, const Previous& b) { return CompareVersions(a.version, b.version) > 0; });
    return out;
}

void SaveRestoreState(const std::wstring& json) { WriteTextFileAtomic(DataDir() + L"\\restore.json", json); }

std::wstring TakeRestoreState() {
    std::wstring p = DataDir() + L"\\restore.json", text;
    if (!ReadTextFile(p, text)) return L"";
    DeleteFileW(p.c_str());
    return text;
}

} // namespace selfupdate
