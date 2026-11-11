// Hangar - Hangar's own updater.
//
// Hangar is one exe that can live anywhere. To update it:
//   1. a background job finds the newest release of larosa023/Hangar on the chosen channel,
//      downloads Hangar.exe and verifies its SHA-256 (GitHub digest + SHA256SUMS.txt)
//   2. when nothing is installing and the update won't interrupt you, the running Hangar saves
//      where you are, renames itself to Hangar.exe.old, puts the new exe in its place, hides its
//      window and starts the new one with --after-update
//   3. the new Hangar opens where you were and signals "healthy"; the old one exits.
//      If the new one doesn't signal within 20 seconds (or quits), the old one puts itself back,
//      shows its window again and skips that version.
//   4. the new Hangar keeps the previous exe in DataDir()\versions\<version>\ (last 2) so you
//      can roll back from Settings.
#pragma once
#include "common.h"

struct SelfUpdateState {
    enum Kind { Idle, Checking, UpToDate, Downloading, Ready, Error } kind = Idle;
    std::wstring version;        // the newer version (Downloading / Ready)
    std::wstring notes;          // its release notes
    std::wstring staged;         // verified exe on disk (Ready)
    std::wstring message;        // Error text
    int percent = -1;
};

namespace selfupdate {

void Check(HWND notify, bool force);        // background; posts WM_HANGAR_SELF updates
// Swap in the staged (or a previous) exe and restart into it. Runs on the UI thread; hides the
// window while the new copy starts. Returns false, with a message, if it stayed on this version.
bool Apply(HWND hwnd, const std::wstring& exe, const std::wstring& version, std::wstring* error);

// Called at startup by the new copy (args from --after-update).
struct AfterUpdate {
    bool active = false;
    std::wstring fromVersion;
    std::wstring healthyEvent;
    DWORD oldPid = 0;
};
AfterUpdate ParseArgs(const wchar_t* cmdLine);
void SignalHealthy(const AfterUpdate& a);    // tell the old copy we started fine
void FinishAfterUpdate(const AfterUpdate& a); // background: keep the previous exe for rollback

struct Previous { std::wstring version, exe; };
std::vector<Previous> PreviousVersions();     // newest first

// Where you were, so the new copy can open on the same page
void SaveRestoreState(const std::wstring& json);
std::wstring TakeRestoreState();

} // namespace selfupdate

// main.cpp: the single-instance mutex is handed over during an update
void ReleaseInstanceMutex();
void ReacquireInstanceMutex();
