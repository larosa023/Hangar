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
- **Release checks (from 0.1.5):** the **Release index** workflow (`.github/workflows/index.yml`, run every 10 minutes and whenever `manifest.json` changes) runs `tools/build_index.py` with the workflow's token. It writes every app's recent releases, including asset SHA-256 digests, to `releases.json` on the `index` branch.
  - Hangar downloads that one file from raw.githubusercontent.com with an `ETag`. That avoids GitHub's API limit of 60 requests an hour for signed-out users, which your apps' own updaters also share.
  - Hangar falls back to `GET /repos/{owner}/{repo}/releases` only for an app the index doesn't cover, or when the index is more than a day old.
- **Verification:** every download is checked with SHA-256 against GitHub's asset digest. When the app publishes a checksum file (`SHA256SUMS.txt`, `Switch.exe.sha256`), Hangar also checks against that, and both must agree. A release with no checksum is refused.
- **Updating an app that's running (Phase 1 fallback):** Hangar closes the app gracefully (with `--quit` if the app supports it, otherwise `WM_CLOSE`). It never kills an app. Then it installs the new version and reopens the app. For single-exe apps the old exe is kept as `.old` until the new one has run for 4 seconds; if the new one fails, Hangar restores the old one and reopens it.

### Hangar's own updater (`selfupdate.cpp`, from 0.1.2)

Hangar is one exe that can live anywhere, so it updates itself like this:
1. A background job checks `larosa023/Hangar` releases (Stable, or Beta for pre-releases), downloads `Hangar.exe` to `updates\<ver>\`, and checks its SHA-256 against both GitHub's digest and `SHA256SUMS.txt`.
2. When you click **Restart to update**, or automatically once Hangar is minimized or the PC has been idle for 2 minutes, and only when no installs are running, Hangar:
   - saves where you are (page, app, search, scroll)
   - renames its exe to `Hangar.exe.old`, copies the new exe into its place and hides its window
   - starts the new exe with `--after-update`
3. The new copy opens on the same page and sets a named "healthy" event once its window is drawn. The old copy then exits.
4. If the event doesn't arrive within 20 s, or the new copy quits, the old copy deletes the new exe, renames itself back, shows its window again, and adds that version to `skipVersion`.
5. The new copy keeps the previous exe in `versions\<ver>\` (the last 2). **Settings → Hangar updates → Roll back** uses the same handoff.

## 4. Project structure

```
Hangar/
├─ manifest.json          the suite catalog (fetched by every Hangar at runtime)
├─ build.sh / CMakeLists.txt
├─ res/                   icon, Windows manifest, version info, embedded fallback manifest
├─ src/
│  ├─ main.cpp            single instance, startup
│  ├─ ui.cpp              window: library, app pages, settings (Direct2D)
│  ├─ gfx.cpp             Direct2D/DirectWrite helpers
│  ├─ themes.cpp          built-in themes, theme files, accent colors
│  ├─ model.cpp           manifest + releases parsing, install detection, process scan
│  ├─ jobs.cpp            background work: fetch, download + verify, install/update/uninstall
│  ├─ selfupdate.cpp      Hangar's own updater: check, stage, handoff, rollback
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

1. **Library, manifest, release checks, install/launch/update/uninstall** ✅ (0.1.0–0.1.1)
2. **Hangar's own updater with rollback** ✅ (0.1.2). Still to do: versioned app installs (`Hangar\apps\<App>\<ver>\` behind a launcher stub), keeping the last N app versions with manual rollback, parallel "Update all" with idle and game rules
3. SDK + IPC + state save/restore + live updates, starting with Strata as the reference
4. Themes: 14 presets, accent colors and custom theme files ✅ (0.1.4; see [THEMES.md](THEMES.md)). Still to do: font, density, corner and animation settings, and suite-wide live theming through the SDK
5. Backup/restore, new-PC setup, health dashboard
6. Publish page with GitHub sign-in (device flow, token kept in Credential Manager)
7. Tray service, notifications, accessibility (UI Automation), performance tuning
