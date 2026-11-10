#!/usr/bin/env bash
# Cross-compile Hangar.exe from Linux/macOS with Zig (pip install ziglang, or a zig binary on PATH).
set -euo pipefail
cd "$(dirname "$0")"
if [[ -z "${ZIG:-}" ]]; then
  if command -v zig >/dev/null 2>&1; then ZIG=zig; else ZIG="python3 -m ziglang"; fi
fi
mkdir -p dist
$ZIG c++ -target x86_64-windows-gnu -std=c++17 -O2 -s -municode \
  -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -Wall -Wno-unknown-pragmas -Wno-missing-field-initializers \
  src/*.cpp res/hangar.rc -o dist/Hangar.exe \
  -Wl,--subsystem,windows \
  -ld2d1 -ldwrite -lole32 -loleaut32 -luuid -ldwmapi -lshcore -lshell32 -luser32 -lgdi32 -ladvapi32 -lshlwapi \
  -lwindowscodecs -lcomctl32 -luxtheme -lwinhttp -lbcrypt -lversion
rm -f dist/Hangar.pdb
ls -la dist/Hangar.exe
