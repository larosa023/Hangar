# Hangar

Home base for the larosa023 suite of Windows apps. From one window, Hangar installs, opens, updates and uninstalls every app in the suite (Foyer, Strata, Breaker, Switch, Isle, Faderly, Tonely, Ember, Marq, Mantel, Lumora, Savor, Driftcord, Amplite, Cinder, Relic, Prism, Ripple, Downtime, Drift, Gridline, Fullcourt…).

**Status: v0.1.2.** Library, app pages, release checks, and one-click install/open/update/repair/uninstall, with verified downloads and automatic rollback. **Hangar updates itself** (Settings → Hangar updates). 14 themes plus custom theme files (Themes page). Live updates, themes, backup and publishing come in later phases: see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Run it

Download `Hangar.exe` from [Releases](https://github.com/larosa023/Hangar/releases) and double-click it. It runs from anywhere and needs no admin rights. Windows may show SmartScreen the first time because the exe isn't signed yet: **More info → Run anyway**.

Portable mode: put an empty file named `portable` next to `Hangar.exe`, and its data goes in a `data` folder beside it.

## Using it

| | |
|---|---|
| **Library** | Grid or list view; filter by category, Installed or Updates in the sidebar |
| **Search** | **Ctrl+K**, or just start typing. Esc clears it |
| **Keyboard** | Arrows move between apps · **Enter** opens an app's page · **Space** runs the main action · **Ctrl+U** updates everything · **F5** checks GitHub now · Esc / Alt+← goes back · Ctrl+, opens Settings |
| **App page** | Install / Update / Open, Open folder, Repair (download again and reinstall), Uninstall, the Stable or Beta channel for that app, and the release history |

**Safety.** Every download is checked against GitHub's SHA-256 digest, and against the app's own checksum file when it has one. If an app is open, Hangar asks first, closes it the normal way (never by force), updates it, and reopens it. If a new single-exe version doesn't start, Hangar puts the previous one back.

**Apps keep working without Hangar.** Hangar installs each app where the app installs itself and the way it installs itself, so every app's own updater keeps working.

## Adding an app

Add one entry to [`manifest.json`](manifest.json) and push. Every copy of Hangar picks it up. See [docs/MANIFEST.md](docs/MANIFEST.md). For apps that aren't public yet, use **Settings → Advanced → Local manifest**.

## Build

```bash
./build.sh                         # Linux/macOS, needs Zig: pip install ziglang
cmake -S . -B build -A x64 && cmake --build build --config Release   # Windows, VS 2022
```

A release is published automatically when `HANGAR_VERSION` in `src/common.h` changes on `main`. Versions containing a `-` are published as pre-releases (the Beta channel).

## Docs

- [Architecture, stack choice, and phases](docs/ARCHITECTURE.md)
- [Manifest format](docs/MANIFEST.md)
- [IPC protocol for live updates](docs/IPC.md)
- [Themes and the theme file format](docs/THEMES.md)
