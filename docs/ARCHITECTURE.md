# Hangar architecture

## 1. Tech stack

**Hangar: native C++17, Win32 + Direct2D/DirectWrite, cross-compiled with Zig.**
It's the same stack and toolchain as Foyer, Strata and Breaker (`./build.sh` on Linux, or CMake + MSVC on Windows).

Why:
- **Footprint.** One ~600 KB exe with no runtime. It starts in well under a second and uses about 15–25 MB of RAM. When idle it uses no CPU: timers only run while something is animating, and the process poll only runs while the window is visible.
- **Reliability.** No framework sits between Hangar and the things it manages (processes, files, the registry, named pipes).
- **Reuse.** The JSON reader, the Direct2D helpers and the GitHub updater conventions already exist in your C++ apps.

Alternatives considered: WPF/.NET (bigger and slower to start), WinUI 3 (heavy runtime and packaging friction), and Tauri/Electron (a WebView costs 80–200 MB, which is too much for something that sits in the tray).

### Velopack?

**Hangar does not use Velopack for its own install model. It does support Velopack apps.**

Four of your apps (Ember, Mantel, Faderly, Tonely) already ship with Velopack, so Hangar installs them by running their `Setup.exe --silent`.

For the suite-wide model the spec asks for, Velopack is the wrong core:
- Velopack keeps one `current\` folder and swaps it on restart. Hangar needs side-by-side version folders (`apps\<App>\<version>\`) so it can stage, hand off live and roll back to any of the last few versions.
- Velopack's packaging (`vpk`) is .NET-centric and wraps each app in its own `Update.exe`. Your C++, Rust, Python, Tauri and Electron apps don't use it.
- The live-update handoff (prepare-update, save state, restart, health ping) is outside what Velopack does in any case.

| | Custom versioned folders (chosen) | Velopack for everything |
|---|---|---|
| Live handoff and keeping the last N versions | Yes | Only one previous version |
| Works the same for all 6 stacks | Yes | Needs a repackage per stack |
| Delta updates | Not yet (the exes are small) | Yes |
| Code to own | About 400 lines in Hangar plus the SDK | Less, but it doesn't do the key feature |

Delta updates can be added later if a large app needs them.

## 2. The suite today

| Install type | Apps | What Hangar does |
|---|---|---|
| `portable` (one exe) | Foyer, Strata, Breaker, Switch, Marq, Savor, Cinder, Relic | Downloads the exe, verifies it, places it in `%LOCALAPPDATA%\Programs\<App>`, adds a Start menu shortcut and an Apps & features entry |
| `nsis` (setup exe) | Amplite, Gridline, Prism, Drift (Electron); Driftcord, Ripple, Downtime, Lumora, Fullcourt (Tauri) | Runs the setup with `/S`, then finds the app through Apps & features |
| `velopack` | Ember, Mantel, Faderly, Tonely | Runs `Setup.exe --silent`, then finds `%LOCALAPPDATA%\<packId>\current\<exe>` |

Hangar installs portable apps in exactly the place the apps install themselves. A copy you installed by hand shows up as installed, and every app keeps updating itself when Hangar isn't there.

## 3. How Hangar works

- The **UI thread** owns all app state. **Background jobs** (`jobs.cpp`) do the network, disk, registry and process work, and post their results back to the window with `PostMessage`. The UI never waits on I/O.
- **Startup:** the library appears immediately from the cached manifest (or the built-in copy) and cached release data. A background refresh follows.
- **Release checks:** `GET /repos/{owner}/{repo}/releases` with an `ETag`, so a 304 doesn't use up GitHub's limit of 60 requests an hour. Hangar only goes to the network when the cache is older than the check interval (default 3 hours). If the limit is hit, it stops until GitHub's reset time.
- **Verification:** every download is checked with SHA-256 against GitHub's asset digest. When the app publishes a checksum file (`SHA256SUMS.txt`, `Switch.exe.sha256`), Hangar also checks against that, and both must agree. A release with no checksum is refused.
- **Updating an app that's running (Phase 1 fallback):** Hangar closes the app gracefully (with `--quit` if the app supports it, otherwise `WM_CLOSE`). It never kills an app. Then it installs the new version and reopens the app. For single-exe apps the old exe is kept as `.old` until the new one has run for 4 seconds; if the new one fails, Hangar restores the old one and reopens it.

## 4. Project structure

```
Hangar/
├─ manifest.json          the suite catalog (fetched by every Hangar at runtime)
├─ build.sh / CMakeLists.txt
├─ res/                   icon, Windows manifest, version info, embedded fallback manifest
├─ src/
│  ├─ main.cpp            single instance, startup
│  ├─ ui.cpp              window: library, app pages, settings (Direct2D)
│  ├─ gfx.cpp             Direct2D/DirectWrite helpers, theme
│  ├─ model.cpp           manifest + releases parsing, install detection, process scan
│  ├─ jobs.cpp            background work: fetch, download + verify, install/update/uninstall
│  ├─ net.cpp             WinHTTP
│  ├─ config.cpp          settings.json
│  ├─ util.cpp            paths, strings, versions, SHA-256, logging
│  └─ json.h
├─ sdk/                   (Phase 3) Hangar SDK, one folder per language
│  ├─ PROTOCOL.md         the spec all ports follow
│  ├─ cpp/  rust/  dotnet/  js/  python/
└─ docs/
```

Data lives in `%LOCALAPPDATA%\Hangar\`: `settings.json`, `hangar.log`, `cache\` (manifest, releases, icons), `downloads\`, and an optional `manifest.local.json`. In portable mode (a `portable` file next to the exe), the same files go in `.\data\`.

## 5. Phases

1. **Library, manifest, release checks, install/launch/update/uninstall** ✅ (this build)
2. Versioned installs (`Hangar\apps\<App>\<ver>\` behind a launcher stub), Hangar's own updater, keeping the last N versions, manual rollback, parallel "Update all" with idle and game rules
3. SDK + IPC + state save/restore + live updates, starting with Strata as the reference
4. Themes and suite-wide live theming
5. Backup/restore, new-PC setup, health dashboard
6. Publish page with GitHub sign-in (device flow, token kept in Credential Manager)
7. Tray service, notifications, accessibility (UI Automation), performance tuning
