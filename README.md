# Window Size Capture

[简体中文](README.zh-CN.md) · [Download](https://github.com/nonenty/obs-window-size-capture/releases/latest)

A Windows OBS Studio plugin that matches windows by **executable name and size**, independently of window titles. It can composite floating windows from the same process and capture process audio.

Tested with **OBS 32.0.4 and 33.0.1, Windows x64**. Video output is 30 fps / SDR. Windows Graphics Capture requires Windows 10 1903 or later; process audio requires Windows build 20348 or later. OBS's own system requirements also apply.

## Install

1. Download `window-size-capture-<version>-windows-x64.zip` from [Releases](https://github.com/nonenty/obs-window-size-capture/releases/latest) and exit OBS.
2. Open the OBS Studio installation folder containing `bin`, `data` and `obs-plugins`.
3. Extract the ZIP's contents into that folder, merge the folders and replace existing plugin files.
4. Start OBS and add a **Window Size Capture** source.

The ZIP mirrors the OBS folder layout:

```text
bin/64bit/window-size-capture-worker.exe
obs-plugins/64bit/window-size-capture.dll
data/obs-plugins/window-size-capture/
```

To uninstall, exit OBS and delete these two files and the plugin's data folder.

## Use

Select a window in the source properties to save its executable name and dimensions. Refresh the list if the window is missing. Client-area capture is enabled by default; to include the title bar, disable it and select the window again. Size tolerance defaults to 4 pixels.

**Capture main window and process windows** composites floating windows at their relative positions, clipped to the main window. **Capture audio** includes the target process and its children; audio cannot be separated by window. Windows with identical executable names and sizes cannot be reliably distinguished.

## Build

Use WSL and MinGW-w64 with OBS installed on Windows:

```bash
sudo apt-get install git python3 mingw-w64
export OBS_BIN='/mnt/c/Program Files/obs-studio/bin/64bit'
./package.sh
./test.sh  # Requires an interactive Windows desktop and graphics hardware
```

The build downloads OBS 32.0.4 headers. `OBS_SDK` and `BUILD_DIR` override the SDK and build directories. The two release ZIPs are written to `build/`; `build/release/` contains the files to copy into OBS. GitHub Actions checks compilation and ZIP contents; local tests check capture pixels, lifecycle, audio and worker recovery.

[Changelog](CHANGELOG.md) · [GPL-2.0-or-later](LICENSE) · [Third-party notices](THIRD_PARTY_NOTICES.md)
