// Hangar - settings
#include "config.h"
#include "util.h"
#include "json.h"

Config g_cfg;

static std::wstring ConfigPath() { return DataDir() + L"\\settings.json"; }

void ConfigLoad() {
    std::wstring text;
    if (!ReadTextFile(ConfigPath(), text)) return;
    json::Value v;
    if (!json::Parser(text).Parse(v) || v.type != json::Value::Object) { Log(L"settings.json is damaged; using defaults"); return; }
    Config c;
    c.theme = v.GetStr(L"theme", c.theme);
    c.accent = v.GetStr(L"accent", c.accent);
    c.listView = v.GetBool(L"listView", c.listView);
    c.beta = v.GetBool(L"beta", c.beta);
    c.checkHours = (int)v.GetNum(L"checkHours", c.checkHours);
    if (c.checkHours < 1) c.checkHours = 1;
    if (c.checkHours > 48) c.checkHours = 48;
    c.page = v.GetStr(L"page", c.page);
    c.hangarBeta = v.GetBool(L"hangarBeta", c.hangarBeta);
    c.hangarAuto = v.GetBool(L"hangarAuto", c.hangarAuto);
    c.skipVersion = v.GetStr(L"skipVersion");
    if (const json::Value* w = v.Get(L"window"); w && w->type == json::Value::Object) {
        c.windowX = (int)w->GetNum(L"x", CW_USEDEFAULT);
        c.windowY = (int)w->GetNum(L"y", CW_USEDEFAULT);
        c.windowW = (int)w->GetNum(L"w", 0);
        c.windowH = (int)w->GetNum(L"h", 0);
        c.maximized = w->GetBool(L"maximized", false);
    }
    if (const json::Value* ch = v.Get(L"channels"); ch && ch->type == json::Value::Object)
        for (auto& kv : ch->obj) if (kv.second.type == json::Value::String) c.channel[kv.first] = kv.second.s;
    g_cfg = c;
}

void ConfigSave() {
    json::Value v = json::Value::MakeObject();
    v.Set(L"theme", json::Value::Str(g_cfg.theme));
    v.Set(L"accent", json::Value::Str(g_cfg.accent));
    v.Set(L"listView", json::Value::Boolean(g_cfg.listView));
    v.Set(L"beta", json::Value::Boolean(g_cfg.beta));
    v.Set(L"checkHours", json::Value::Num(g_cfg.checkHours));
    v.Set(L"page", json::Value::Str(g_cfg.page));
    v.Set(L"hangarBeta", json::Value::Boolean(g_cfg.hangarBeta));
    v.Set(L"hangarAuto", json::Value::Boolean(g_cfg.hangarAuto));
    v.Set(L"skipVersion", json::Value::Str(g_cfg.skipVersion));
    json::Value w = json::Value::MakeObject();
    w.Set(L"x", json::Value::Num(g_cfg.windowX));
    w.Set(L"y", json::Value::Num(g_cfg.windowY));
    w.Set(L"w", json::Value::Num(g_cfg.windowW));
    w.Set(L"h", json::Value::Num(g_cfg.windowH));
    w.Set(L"maximized", json::Value::Boolean(g_cfg.maximized));
    v.Set(L"window", w);
    json::Value ch = json::Value::MakeObject();
    for (auto& kv : g_cfg.channel) ch.Set(kv.first, json::Value::Str(kv.second));
    v.Set(L"channels", ch);
    std::wstring out;
    json::Write(v, out);
    if (!WriteTextFileAtomic(ConfigPath(), out)) Log(L"couldn't save settings.json");
}

bool AppUsesBeta(const std::wstring& id) {
    auto it = g_cfg.channel.find(id);
    if (it != g_cfg.channel.end()) return it->second == L"beta";
    return g_cfg.beta;
}

void SetAppChannel(const std::wstring& id, bool beta) {
    if (beta == g_cfg.beta) g_cfg.channel.erase(id);
    else g_cfg.channel[id] = beta ? L"beta" : L"stable";
}

static std::vector<std::wstring> SkipList() {
    std::vector<std::wstring> out;
    size_t i = 0;
    const std::wstring& s = g_cfg.skipVersion;
    while (i < s.size()) {
        size_t e = s.find(L',', i);
        if (e == std::wstring::npos) e = s.size();
        std::wstring v = Trim(s.substr(i, e - i));
        if (!v.empty()) out.push_back(v);
        i = e + 1;
    }
    return out;
}

static void SetSkipList(const std::vector<std::wstring>& list) {
    g_cfg.skipVersion.clear();
    for (auto& v : list) g_cfg.skipVersion += (g_cfg.skipVersion.empty() ? L"" : L",") + v;
}

bool IsSkippedVersion(const std::wstring& v) {
    for (auto& s : SkipList()) if (CompareVersions(s, v) == 0) return true;
    return false;
}

void SkipVersion(const std::wstring& v) {
    if (IsSkippedVersion(v)) return;
    auto list = SkipList();
    list.push_back(NormalizeVersion(v));
    SetSkipList(list);
}

void PruneSkippedVersions(const std::wstring& current) {
    auto list = SkipList();
    std::vector<std::wstring> keep;
    for (auto& v : list) if (CompareVersions(v, current) > 0) keep.push_back(v);
    SetSkipList(keep);
}
