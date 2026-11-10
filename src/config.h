// Hangar - settings (DataDir()\settings.json)
#pragma once
#include "common.h"
#include <map>

struct Config {
    std::wstring theme = L"dark";       // dark | light | system
    bool listView = false;
    bool beta = false;                  // default channel for every app
    std::map<std::wstring, std::wstring> channel;   // per-app override: id -> "stable" | "beta"
    int checkHours = 3;                 // how often release checks run in the background
    int windowX = CW_USEDEFAULT, windowY = CW_USEDEFAULT, windowW = 0, windowH = 0;
    bool maximized = false;
    std::wstring page = L"all";         // last library filter
};

extern Config g_cfg;
void ConfigLoad();
void ConfigSave();
bool AppUsesBeta(const std::wstring& id);
void SetAppChannel(const std::wstring& id, bool beta);
