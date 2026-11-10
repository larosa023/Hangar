# Hangar IPC protocol (v1, draft; implemented in Phase 3)

Hangar talks to each running app over a **named pipe**. The protocol is plain JSON lines, so every stack in the suite (C++, Rust, C#, Tauri/JS, Python) can implement it in about 100 lines. The SDK ships a port for each.

## Transport

- **Pipe:** `\\.\pipe\larosa023.<appId>.<sessionId>`. The app is the server. `sessionId` keeps two Windows users apart.
- **Access:** the current user only (the default DACL plus `PIPE_REJECT_REMOTE_CLIENTS`).
- **Framing:** one UTF-8 JSON object per line (`\n`), each at most 1 MB.
- **Requests:** every request has `id` (a number) and `type`. Every reply echoes `id`.
- **Discovery:** an app with the SDK also writes `%LOCALAPPDATA%\Hangar\run\<appId>.json` containing `{pid, pipe, version, protocol: 1}`. That way Hangar never has to guess whether an app speaks the protocol.

## Messages: Hangar → app

| type | reply | meaning |
|---|---|---|
| `hello` | `{protocol:1, app, version, capabilities:["state","theme","settings"]}` | handshake |
| `status` | `{state:"idle"}` or `{state:"busy", reason:"file transfer", until?:<unix>}` | Hangar only restarts an app that is idle |
| `prepare-update` | `{ok:true, stateFile}` or `{ok:false, reason}` | the app saves everything it needs to come back exactly where it was (tabs, panes, page, window position, unsaved input) to `stateFile` and stops accepting new work |
| `restart` | (pipe closes) | the app exits. The launcher starts the new version with `--restore-state <stateFile>` |
| `cancel-update` | `{ok:true}` | undoes `prepare-update` (for example, Hangar's download failed) |
| `apply-theme` | `{ok:true}` | `{theme:{…}}` in the shared theme format (Phase 4). Applied live, with no restart |
| `apply-settings` | `{ok:true, restartRequired:false}` | hot-reloads settings changed by a restore |
| `export-settings` | `{file}` | writes the app's settings in the shared backup format (Phase 5) |

## Messages: app → Hangar

Hangar's own pipe is `\\.\pipe\larosa023.hangar.<sessionId>`.

| type | meaning |
|---|---|
| `health` `{app, version, pid}` | sent about 2 s after a successful start. **This is what confirms an update.** If it doesn't arrive within 20 s, or the app crashes first, Hangar rolls back |
| `busy` / `idle` `{reason}` | optional push, so Hangar doesn't need to poll |
| `restored` `{summary:"3 tabs"}` | shown in the live progress view ("restarted, restored 3 tabs") |

## Live-update sequence

```
Hangar                                   App (old)                 Launcher / App (new)
  │ download → verify → stage apps\Strata\0.4.0\
  │──status──────────────────────────────▶│
  │◀─────────────── {state:"busy", reason:"file transfer"}   (wait, retry every 5 s)
  │──status──────────────────────────────▶│
  │◀───────────────────────────── {state:"idle"}
  │──prepare-update──────────────────────▶│ saves state.json
  │◀──────────────────────── {ok, stateFile}
  │──restart─────────────────────────────▶│ exits
  │                                        stub starts 0.4.0 --restore-state …
  │◀──────────────────────────────────────────────────── health {version:"0.4.0"}
  │◀──────────────────────────────────────────────────── restored {summary:"3 tabs"}
  │ mark 0.4.0 good, keep the previous 2 versions
```

## Apps without the SDK

Hangar falls back to what Phase 1 already does: close the app gracefully (`--quit`, then `WM_CLOSE`), swap in the new version, reopen it, and watch for a crash on launch.

## Rules

- Hangar never sends `restart` while a game or full-screen app is in front, or while an app reports `busy`.
- Critical apps (Foyer, Faderly, Switch, Strata) default to the "When idle" update mode.
- An app may always refuse `prepare-update`. Hangar treats that as busy.
