#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

plugin_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
obs_bin="${OBS_BIN:-/mnt/c/Program Files/obs-studio/bin/64bit}"
build_dir="${BUILD_DIR:-$plugin_dir/build}"
obs_sdk="${OBS_SDK:-$build_dir/obs-sdk}"

"$plugin_dir/build.sh"
for test_name in test_floating_windows test_composite test_lifecycle; do
    x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
        -I "$obs_sdk/libobs" -I "$build_dir/include" \
        -c -o "$build_dir/$test_name.o" "$plugin_dir/tests/$test_name.c"
    x86_64-w64-mingw32-g++ -o "$build_dir/$test_name.exe" \
        "$build_dir/$test_name.o" "$build_dir/capture-host.o" "$build_dir/process-audio.o" \
        -L "$obs_bin" -l:obs.dll -ldwmapi -luser32 -lgdi32 -lkernel32 -lole32 -luuid \
        -static-libgcc -static-libstdc++ -Wl,--no-undefined

    powershell.exe -NoProfile -ExecutionPolicy Bypass \
        -File "$(wslpath -w "$plugin_dir/tests/run.ps1")" \
        -ObsBin "$(wslpath -w "$obs_bin")" \
        -TestExe "$(wslpath -w "$build_dir/$test_name.exe")"
done

x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -Werror \
    -I "$obs_sdk/libobs" -I "$build_dir/include" \
    -o "$build_dir/test_audio_stop.exe" "$plugin_dir/tests/test_audio_stop.cpp" \
    -L "$obs_bin" -l:obs.dll -lole32 -luuid -static-libgcc -static-libstdc++ -Wl,--no-undefined
powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -File "$(wslpath -w "$plugin_dir/tests/run.ps1")" \
    -ObsBin "$(wslpath -w "$obs_bin")" \
    -TestExe "$(wslpath -w "$build_dir/test_audio_stop.exe")"

# Fault injection is built into this test helper only, never the shipped EXE.
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -DCAPTURE_TEST_HOOKS \
    -I "$obs_sdk/libobs" -I "$build_dir/include" \
    -c -o "$build_dir/capture-worker-test.o" "$plugin_dir/capture-worker.c"
x86_64-w64-mingw32-g++ -o "$build_dir/window-size-capture-worker-test.exe" \
    "$build_dir/capture-worker-test.o" "$build_dir/capture-host.o" "$build_dir/process-audio.o" \
    -L "$obs_bin" -l:obs.dll -ldwmapi -ldxgi -luser32 -lkernel32 -lole32 -luuid \
    -static-libgcc -static-libstdc++ -Wl,--no-undefined -municode
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror \
    -I "$obs_sdk/libobs" -I "$build_dir/include" \
    -c -o "$build_dir/test_isolation.o" "$plugin_dir/tests/test_isolation.c"
x86_64-w64-mingw32-g++ -o "$build_dir/test_isolation.exe" \
    "$build_dir/test_isolation.o" "$build_dir/process-audio.o" \
    -L "$obs_bin" -l:obs.dll -ldwmapi -luser32 -lgdi32 -lkernel32 -lole32 -luuid \
    -static-libgcc -static-libstdc++ -Wl,--no-undefined
powershell.exe -NoProfile -ExecutionPolicy Bypass \
    -File "$(wslpath -w "$plugin_dir/tests/run.ps1")" \
    -ObsBin "$(wslpath -w "$obs_bin")" \
    -TestExe "$(wslpath -w "$build_dir/test_isolation.exe")" \
    -WorkerExe "$(wslpath -w "$build_dir/window-size-capture-worker-test.exe")" \
    -TimeoutSeconds 90

"$plugin_dir/package.sh"
python3 "$plugin_dir/tests/test_package.py" "$build_dir"
