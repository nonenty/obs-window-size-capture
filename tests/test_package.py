"""Check release ZIPs and copying their contents over an existing OBS tree."""
from pathlib import Path, PurePosixPath
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else ROOT / "build"
VERSION = (ROOT / "VERSION").read_text().strip()
DATA = "data/obs-plugins/window-size-capture/"


class PackageTests(unittest.TestCase):
    def test_binary_layout(self):
        with zipfile.ZipFile(BUILD / f"window-size-capture-{VERSION}-windows-x64.zip") as archive:
            self.assertIsNone(archive.testzip())
            self.assertEqual(set(archive.namelist()), {
                "bin/64bit/window-size-capture-worker.exe",
                "obs-plugins/64bit/window-size-capture.dll",
                DATA + "locale/en-US.ini", DATA + "locale/zh-CN.ini",
                DATA + "LICENSE", DATA + "THIRD_PARTY_NOTICES.md",
                DATA + "licenses/GPL-3.0.txt",
                DATA + "licenses/GCC-Runtime-Library-Exception-3.1.txt",
                DATA + "licenses/MinGW-w64-copyright.txt",
            })

    def test_extract_and_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            obs = Path(directory)
            existing = {
                "bin/64bit/obs64.exe": b"existing OBS executable",
                "data/obs-studio/global.ini": b"existing settings",
                "obs-plugins/64bit/another-plugin.dll": b"another plugin",
            }
            for name, contents in {**existing,
                                   "obs-plugins/64bit/window-size-capture.dll": b"old plugin",
                                   "bin/64bit/window-size-capture-worker.exe": b"old worker"}.items():
                path = obs / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(contents)
            with zipfile.ZipFile(BUILD / f"window-size-capture-{VERSION}-windows-x64.zip") as archive:
                for name in archive.namelist():
                    self.assertFalse(PurePosixPath(name).is_absolute())
                    self.assertNotIn("..", PurePosixPath(name).parts)
                archive.extractall(obs)
                for name in archive.namelist():
                    self.assertEqual((obs / name).read_bytes(), archive.read(name))
            for name, contents in existing.items():
                self.assertEqual((obs / name).read_bytes(), contents)

    def test_matching_source_archive(self):
        prefix = f"window-size-capture-{VERSION}/"
        with zipfile.ZipFile(BUILD / f"window-size-capture-{VERSION}-source.zip") as archive:
            self.assertIsNone(archive.testzip())
            for required in ["README.md", "README.zh-CN.md", "LICENSE", "VERSION",
                             "package.sh", "tests/test_package.py", ".github/workflows/build.yml"]:
                self.assertIn(prefix + required, archive.namelist())
            for name in archive.namelist():
                self.assertTrue(name.startswith(prefix))
                relative = PurePosixPath(name.removeprefix(prefix))
                self.assertFalse(relative.is_absolute())
                self.assertFalse(set(relative.parts) & {"..", ".git", "build", "dist"})
                self.assertNotEqual(relative.name, "install.ps1")
                self.assertEqual(archive.read(name), (ROOT / relative).read_bytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)
