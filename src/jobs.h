// Hangar - background work. Every job runs off the UI thread and posts its result to the
// main window, which owns all app state. Nothing here touches the UI.
#pragma once
#include "common.h"
#include "model.h"

struct ManifestResult {
    bool changed = false;          // a newer manifest than the one we started with
    std::wstring text, error;
};

struct ReleasesResult {
    std::wstring id;
    bool ok = false;
    bool fromCache = false;        // cached copy (startup), network refresh still to come
    std::vector<ReleaseInfo> releases;
    std::wstring error;
    long long checkedAt = 0;       // unix seconds of the last successful check
};

struct IconResult {
    std::wstring id;
    UINT w = 0, h = 0;
    std::vector<uint8_t> pixels;   // premultiplied BGRA, w*h*4
};

struct ScanResult {
    std::vector<std::pair<std::wstring, Installed>> items;
};

struct ProcResult {
    std::vector<std::wstring> ids;
    std::vector<std::vector<DWORD>> pids;
};

enum class OpKind { Install, Update, Repair, Uninstall };

struct OpRequest {
    OpKind kind = OpKind::Install;
    AppDef def;
    Installed inst;
    ReleaseInfo release;           // install / update / repair
    bool beta = false;
};

struct OpProgress {
    std::wstring id;
    int percent = -1;              // -1: indeterminate
    std::wstring text;
};

struct OpDone {
    std::wstring id;
    OpKind kind = OpKind::Install;
    bool ok = false;
    bool cancelled = false;
    std::wstring message;          // shown to the user
    std::wstring version;
};

namespace jobs {

void Init(HWND notify);
void Shutdown();                   // cancels downloads; workers exit on their own

void FetchManifest(const std::wstring& currentText);
void LoadCachedReleases(const AppDef& def);
void FetchReleases(const AppDef& def, bool force);
void FetchIcon(const AppDef& def, UINT px);
void Scan(const std::vector<AppDef>& defs);
void ScanProcesses(const std::vector<std::wstring>& ids, const std::vector<std::wstring>& exes, const std::vector<std::wstring>& dirs);
void StartOp(const OpRequest& req);
void CancelOp(const std::wstring& id);
int ActiveOps();

long long RateLimitedUntil();      // unix seconds; 0 when not rate limited

} // namespace jobs
