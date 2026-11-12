#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

plugin_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
obs_bin="${OBS_BIN:-/mnt/c/Program Files/obs-studio/bin/64bit}"
build_dir="${BUILD_DIR:-$plugin_dir/build}"
obs_sdk="${OBS_SDK:-$build_dir/obs-sdk}"
version="$(cat "$plugin_dir/VERSION")"

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Invalid plugin VERSION: $version" >&2
    exit 1
fi
for tool in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++; do
    command -v "$tool" >/dev/null || { echo "Required tool missing: $tool" >&2; exit 1; }
done
if [[ ! -f "$obs_bin/obs.dll" ]]; then
    echo "OBS DLL missing: $obs_bin/obs.dll" >&2
    echo 'Set OBS_BIN to the WSL path of your OBS Studio 32.0.4 or 33.0.1 bin/64bit directory.' >&2
    exit 1
fi
if [[ ! -f "$obs_sdk/libobs/obs-config.h" ]]; then
    mkdir -p "$(dirname "$obs_sdk")"
    git clone --quiet --depth 1 --branch 32.0.4 --filter=blob:none \
        https://github.com/obsproject/obs-studio.git "$obs_sdk"
fi

mkdir -p "$build_dir/include"
printf '#pragma once\n#define WINDOW_SIZE_CAPTURE_VERSION "%s"\n' "$version" > "$build_dir/include/plugin-version.h"
cat > "$build_dir/include/obsconfig.h" <<'HEADER'
#pragma once
#define OBS_DATA_PATH "data"
#define OBS_PLUGIN_PATH "obs-plugins"
#define OBS_PLUGIN_DESTINATION "obs-plugins"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
HEADER

x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
    -I "$obs_sdk/libobs" -I "$build_dir/include" \
    -c -o "$build_dir/window-size-capture.o" "$plugin_dir/window-size-capture.c"
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror \
    -I "$obs_sdk/libobs" -I "$build_dir/include" \
    -c -o "$build_dir/process-audio.o" "$plugin_dir/process-audio.cpp"
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
    -I "$obs_sdk/libobs" -I "$build_dir/include" \
    -c -o "$build_dir/capture-host.o" "$plugin_dir/capture-host.c"
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
    -I "$obs_sdk/libobs" -I "$build_dir/include" \
    -c -o "$build_dir/capture-worker.o" "$plugin_dir/capture-worker.c"
x86_64-w64-mingw32-g++ -o "$build_dir/window-size-capture-worker.exe" \
    "$build_dir/capture-worker.o" "$build_dir/capture-host.o" "$build_dir/process-audio.o" \
    -L "$obs_bin" -l:obs.dll -ldwmapi -ldxgi -luser32 -lkernel32 -lole32 -luuid \
    -static-libgcc -static-libstdc++ -Wl,--no-undefined -municode
x86_64-w64-mingw32-g++ -shared -o "$build_dir/window-size-capture.dll" \
    "$build_dir/window-size-capture.o" "$build_dir/capture-host.o" "$build_dir/process-audio.o" \
    -L "$obs_bin" -l:obs.dll -ldwmapi -luser32 -lkernel32 -lole32 -luuid \
    -static-libgcc -static-libstdc++ -Wl,--no-undefined

echo "Built Window Size Capture $version: $build_dir/window-size-capture.dll and window-size-capture-worker.exe"
