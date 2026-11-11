#!/usr/bin/env bash
# Copies the version from src/common.h (HANGAR_VERSION) into the exe's version resource and manifest,
# so Windows (and Hangar) always see the same version the code reports.
set -euo pipefail
cd "$(dirname "$0")"
VER=$(grep -oP 'HANGAR_VERSION L"\K[^"]+' src/common.h)
NUM=${VER%%-*}
IFS=. read -r MA MI PA <<< "$NUM"
MA=${MA:-0}; MI=${MI:-0}; PA=${PA:-0}
sed -i -E "s/^( FILEVERSION ).*/\1$MA,$MI,$PA,0/; s/^( PRODUCTVERSION ).*/\1$MA,$MI,$PA,0/; s/(VALUE \"FileVersion\", )\"[^\"]*\"/\1\"$VER\"/; s/(VALUE \"ProductVersion\", )\"[^\"]*\"/\1\"$VER\"/" res/hangar.rc
sed -i -E "s/(<assemblyIdentity type=\"win32\" name=\"larosa023.Hangar\" version=\")[^\"]*\"/\1$MA.$MI.$PA.0\"/" res/hangar.manifest
echo "Version $VER stamped into res/hangar.rc"
