# The app manifest

`manifest.json` at the root of this repo is the suite's catalog. Every copy of Hangar downloads it
from `https://raw.githubusercontent.com/larosa023/Hangar/main/manifest.json` on startup and every
few hours, and keeps a cached copy (plus a copy built into the exe) for working offline.

**Adding an app to the suite = adding one entry here and pushing.** No Hangar code changes.

## Format

```json
{
  "schema": 1,
  "categories": [ { "id": "windows", "name": "Windows replacements" } ],
  "apps": [
    {
      "id": "strata",                       // unique, lowercase, never changes
      "name": "Strata",
      "repo": "larosa023/Strata",           // GitHub Releases are read from here
      "description": "A fast, keyboard-first File Explorer replacement.",
      "category": "windows",
      "icon": "https://raw.githubusercontent.com/larosa023/Strata/main/res/strata.png",  // png or ico
      "exe": "Strata.exe",                  // main executable once installed
      "install": {
        "type": "portable",                 // portable | nsis | velopack
        "asset": "Strata.exe",              // release asset; * wildcards allowed
        "checksums": "SHA256SUMS.txt",      // optional: extra checksum file in the release
        "dir": "%LOCALAPPDATA%\\Programs\\Strata"   // portable only
      },
      "args": { "quit": "--quit", "uninstall": "--uninstall" },   // optional
      "critical": true,                     // optional: core desktop app
      "tagPrefix": "v",                     // optional (Fullcourt uses "desktop-v")
      "minHangar": "0.1.0",                 // optional: hide Install on older Hangars
      "liveUpdate": false,                  // optional: speaks the Hangar IPC protocol (Phase 3)
      "screenshots": []                     // optional: image URLs for the app page
    }
  ]
}
```

### `install` by type

| type | fields | notes |
|---|---|---|
| `portable` | `asset`, `dir`, `checksums` | Hangar copies the exe into `dir` and registers a Start menu shortcut (plus Apps & features if `args.uninstall` is set) |
| `nsis` | `asset`, `registry`, `silentArgs` (default `/S`) | `registry` is the Apps & features display name. "Amplite" also matches electron-builder's "Amplite 1.15.0" |
| `velopack` | `packId`, `asset` (default `<packId>-win-Setup.exe`), `betaAsset`, `silentArgs` (default `--silent`) | Hangar finds the app at `%LOCALAPPDATA%\<packId>\current\<exe>` |

### Release conventions Hangar expects

- The tag is the version: `v1.2.3`, or `<tagPrefix>1.2.3`.
- **Stable channel** = normal releases. **Beta channel** = pre-releases (or a version with a `-`, like `1.3.0-beta.1`). Beta also sees stable releases, whichever is newer.
- Every asset gets a SHA-256 digest from GitHub automatically. If you also publish a checksum file, Hangar checks against both.
- Drafts are ignored.

## Local overrides

`%LOCALAPPDATA%\Hangar\manifest.local.json` has the same format. Its entries are merged by `id`: they replace an existing app's entry or add a new one. Use it for apps that aren't public yet. **Settings → Advanced → Local manifest → Edit** creates a starter file for you. Press Refresh after saving it.

## Testing against a local server

`HANGAR_MANIFEST_URL` and `HANGAR_API_BASE` (normally `https://api.github.com`) point Hangar at a test server instead of GitHub.
