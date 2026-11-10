// Hangar - home base for the larosa023 suite
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

#define HANGAR_VERSION L"0.1.1"
#define HANGAR_REPO_OWNER L"larosa023"
#define HANGAR_REPO_NAME L"Hangar"
#define HANGAR_MANIFEST_URL L"https://raw.githubusercontent.com/larosa023/Hangar/main/manifest.json"

// Messages posted from worker threads to the main window. lParam owns a heap object
// that the window procedure takes over and deletes.
enum : UINT {
    WM_HANGAR_MANIFEST = WM_APP + 1,   // lParam: ManifestResult*
    WM_HANGAR_RELEASES,                // lParam: ReleasesResult*
    WM_HANGAR_ICON,                    // lParam: IconResult*
    WM_HANGAR_SCAN,                    // lParam: ScanResult*
    WM_HANGAR_PROCS,                   // lParam: ProcResult*
    WM_HANGAR_OP_PROGRESS,             // lParam: OpProgress*
    WM_HANGAR_OP_DONE,                 // lParam: OpDone*
    WM_HANGAR_ACTIVATE,                // another Hangar.exe asked us to come to the front
};
