// Hangar - home base for the larosa023 suite of Windows apps
//
//   Hangar.exe              open Hangar (or bring the running one to the front)
//   Hangar.exe --version    print nothing, exit with code 0 (used by smoke tests)
#include "common.h"
#include "ui.h"
#include "util.h"
#include "config.h"
#include "net.h"
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

static HANDLE g_mutex;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int show) {
    if (wcsstr(cmdLine, L"--version")) return 0;

    g_mutex = CreateMutexW(nullptr, TRUE, L"Local\\Hangar.larosa023.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"Hangar.larosa023.Main", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, WM_HANGAR_ACTIVATE, 0, 0);
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetCurrentProcessExplicitAppUserModelID(L"larosa023.Hangar");
    LogInit();
    Log(L"==== Hangar %ls starting (data: %ls%ls)", HANGAR_VERSION, DataDir().c_str(), IsPortable() ? L", portable" : L"");
    ConfigLoad();
    net::Init();

    HWND hwnd = UiCreate(inst, show);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Hangar couldn't start. Details are in hangar.log.", L"Hangar", MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    net::Shutdown();
    Log(L"Hangar exiting");
    CoUninitialize();
    if (g_mutex) CloseHandle(g_mutex);
    return 0;
}
