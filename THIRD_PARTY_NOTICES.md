# Third-party notices

The plugin's own sources are licensed under GPL-2.0-or-later; see [LICENSE](LICENSE).

## OBS Studio

The plugin builds against OBS Studio 32.0.4 headers and uses libobs, its D3D11 renderer and WinRT capture backend. OBS Studio is licensed under GPL-2.0-or-later. OBS libraries are provided by the user's existing installation and are not bundled in this release.

Source and copyright notices: <https://github.com/obsproject/obs-studio/tree/32.0.4>.

## GCC runtime libraries

The MinGW-w64 GCC build statically links libgcc and libstdc++. These runtime libraries are copyright Free Software Foundation, Inc. and licensed under GPL-3.0-or-later with the GCC Runtime Library Exception 3.1. Copies are included:

- [GNU GPL version 3](licenses/GPL-3.0.txt)
- [GCC Runtime Library Exception 3.1](licenses/GCC-Runtime-Library-Exception-3.1.txt)

Source: <https://gcc.gnu.org/>. The runtime exception permits eligible compiled combinations under the terms described in that exception; it does not change the license of the plugin's own sources.

## MinGW-w64

The build uses MinGW-w64 headers and startup/runtime code. Its components carry several licenses and copyright notices. The [MinGW-w64 copyright file](licenses/MinGW-w64-copyright.txt), preserved from the build environment's distribution package, includes those notices and terms. The file describes the broader toolchain; not all listed components are linked into this plugin.

Source: <https://www.mingw-w64.org/>.

## Windows

Windows supplies the Windows Graphics Capture, process loopback audio and other platform APIs. Windows system libraries are not bundled.
