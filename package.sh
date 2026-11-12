#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
plugin_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${BUILD_DIR:-$plugin_dir/build}"
command -v python3 >/dev/null || { echo 'Required tool missing: python3' >&2; exit 1; }
"$plugin_dir/build.sh"
package_dir="$build_dir/release"
staging_dir="$(mktemp -d "$build_dir/.release.XXXXXX")"
trap 'rm -rf -- "$staging_dir"' EXIT
payload="$staging_dir/obs"
plugin_data="$payload/data/obs-plugins/window-size-capture"
mkdir -p "$payload/bin/64bit" "$payload/obs-plugins/64bit" "$plugin_data/locale"
cp "$build_dir/window-size-capture-worker.exe" "$payload/bin/64bit/"
cp "$build_dir/window-size-capture.dll" "$payload/obs-plugins/64bit/"
cp "$plugin_dir/data/locale/"*.ini "$plugin_data/locale/"
cp "$plugin_dir/LICENSE" "$plugin_dir/THIRD_PARTY_NOTICES.md" "$plugin_data/"
cp -R "$plugin_dir/licenses" "$plugin_data/"
python3 - "$payload" "$plugin_dir" "$staging_dir" <<'PY'
import pathlib
import sys
import zipfile

payload, plugin, staging = map(pathlib.Path, sys.argv[1:])
version = (plugin / 'VERSION').read_text().strip()
with zipfile.ZipFile(staging / f'window-size-capture-{version}-windows-x64.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(payload.rglob('*')):
        if path.is_file():
            archive.write(path, path.relative_to(payload))

source_files = {plugin / name for name in [
    '.gitignore', 'VERSION', 'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.md', 'README.zh-CN.md',
    'CHANGELOG.md', 'build.sh', 'package.sh', 'test.sh', 'list-windows.ps1',
]}
for pattern in ['*.c', '*.h', '*.cpp', '*.hpp', 'tests/*.c', 'tests/*.cpp', 'tests/*.ps1',
                'tests/*.py', 'data/locale/*.ini', 'licenses/*.txt', '.github/workflows/*.yml']:
    source_files.update(plugin.glob(pattern))
with zipfile.ZipFile(staging / f'window-size-capture-{version}-source.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    for path in sorted(source_files):
        archive.write(path, pathlib.PurePosixPath(f'window-size-capture-{version}') / path.relative_to(plugin))
PY
rm -rf -- "$package_dir"
mv -- "$payload" "$package_dir"
mv -- "$staging_dir/"*.zip "$build_dir/"
printf 'OBS directory layout: %s\nRelease ZIPs: %s\n' "$package_dir" "$build_dir"
