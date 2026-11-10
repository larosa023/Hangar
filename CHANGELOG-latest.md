## Hangar 0.1.0 (Phase 1)

The first build of Hangar, home base for the larosa023 apps.

- **Library** of all 21 apps in the suite, in a grid or a list, with categories, search (Ctrl+K) and keyboard navigation. It opens straight away from a cache, then refreshes in the background.
- **Shows what's installed and running**, including apps you installed before Hangar existed.
- **Release checks** against each app's GitHub Releases, on the Stable or Beta channel, with very little use of GitHub's rate limit.
- **One-click Install, Open, Update, Repair and Uninstall.** It handles single-exe apps, NSIS setups (Electron and Tauri apps) and Velopack setups.
- **Verified downloads:** SHA-256 against GitHub's digest and the app's own checksum file.
- **Updating an open app:** Hangar closes it the normal way, updates it, and reopens it. If the new version doesn't start, the previous one comes back.
- **Update all**, a page per app with release history, and Settings (theme, default channel, check interval, local manifest).
