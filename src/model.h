// Hangar - the app catalog: manifest, releases, install detection, running processes
#pragma once
#include "common.h"
#include "json.h"

enum class InstallType { Portable, Nsis, Velopack };

struct AppDef {
    std::wstring id, name, repo, owner, repoName, description, category, icon, exe;
    std::wstring tagPrefix = L"v";
    std::wstring minHangar;
    std::vector<std::wstring> screenshots;
    bool critical = false;          // core desktop app (Phase 2: defaults to "When idle" updates)
    bool liveUpdate = false;        // speaks the Hangar IPC protocol (Phase 3)
    bool local = false;             // came from manifest.local.json
    InstallType type = InstallType::Portable;
    std::wstring asset, betaAsset;  // asset name or wildcard ("Prism-Setup-*.exe")
    std::wstring checksums;         // optional checksum file asset ("SHA256SUMS.txt")
    std::wstring dir;               // portable: install folder (environment variables allowed)
    std::wstring registry;          // nsis: Apps & features DisplayName to match
    std::wstring packId;            // velopack: package id
    std::wstring silentArgs;        // nsis "/S", velopack "--silent"
    std::wstring quitArg, uninstallArg;
};

struct Category { std::wstring id, name; };

struct Manifest {
    std::vector<Category> categories;
    std::vector<AppDef> apps;
};

bool ParseManifest(const std::wstring& text, Manifest& m, std::wstring* error, bool local = false);
void MergeManifest(Manifest& base, const Manifest& overrides);   // by id: replaces or adds
std::wstring InstallTypeName(InstallType t);

// ---------------------------------------------------------------- releases
struct Asset {
    std::wstring name, url, sha256;   // sha256 from GitHub's asset digest (may be empty)
    uint64_t size = 0;
};

struct ReleaseInfo {
    std::wstring tag, version, title, notes, published, page;
    bool prerelease = false;
    std::vector<Asset> assets;
};

// Parses /repos/{owner}/{repo}/releases. Skips drafts and tags that don't match the app's prefix.
// Sorted newest version first.
bool ParseReleases(const std::string& body, const AppDef& def, std::vector<ReleaseInfo>& out, std::wstring* error);
const Asset* FindInstallAsset(const ReleaseInfo& r, const AppDef& def, bool beta);
const Asset* FindAsset(const ReleaseInfo& r, const std::wstring& name);
// Newest release with an installable asset on the channel (beta includes stable releases).
const ReleaseInfo* LatestRelease(const std::vector<ReleaseInfo>& list, const AppDef& def, bool beta);

// ---------------------------------------------------------------- installed copies
struct Installed {
    bool installed = false;
    std::wstring version;           // may be empty when unknown
    std::wstring exe;               // full path to the main exe
    std::wstring dir;
    std::wstring uninstallCmd, quietUninstallCmd;
    std::wstring regKey;            // Apps & features key path (under HKCU or HKLM)
    bool machineWide = false;
};

Installed DetectInstalled(const AppDef& def);
std::wstring PortableDir(const AppDef& def);

// ---------------------------------------------------------------- running processes
// For each exe path, the ids of processes running from that path (or, if the path is empty, none).
std::vector<std::vector<DWORD>> FindRunning(const std::vector<std::wstring>& exePaths, const std::vector<std::wstring>& dirs);
