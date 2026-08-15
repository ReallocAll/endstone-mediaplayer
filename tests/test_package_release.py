import sys
import subprocess
import tempfile
import unittest
from pathlib import Path
import tarfile
import zipfile


SOURCE_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE_ROOT / "tools"))

import package_release


class PackageReleaseTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        self.source = self.root / "source"
        for relative in package_release.SDK_FILES:
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(f"fixture:{relative}\n".encode())
        self.wheel = self.root / package_release.WHEEL_NAME
        self._write_wheel(self.wheel)
        self.dll = self.root / "endstone_mediaplayer.dll"
        self.pdb = self.root / "endstone_mediaplayer.pdb"
        self.so = self.root / "endstone_mediaplayer.so"
        self.dll.write_bytes(b"windows dll")
        self.pdb.write_bytes(b"windows pdb")
        self.so.write_bytes(b"linux so")
        self.output = self.root / "dist"

    def tearDown(self):
        self.directory.cleanup()

    @staticmethod
    def _write_wheel(
        path, metadata_version="0.1.0", metadata_license="GPL-3.0-only"
    ):
        metadata = (
            "Metadata-Version: 2.1\n"
            "Name: endstone-mediaplayer\n"
            f"Version: {metadata_version}\n"
            f"License: {metadata_license}\n"
        ).encode()
        wheel_metadata = "Wheel-Version: 1.0\nGenerator: test\nRoot-Is-Purelib: true\n".encode()
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("endstone_mediaplayer/__init__.py", b"# fixture\n")
            archive.writestr("endstone_mediaplayer-0.1.0.dist-info/METADATA", metadata)
            archive.writestr("endstone_mediaplayer-0.1.0.dist-info/WHEEL", wheel_metadata)

    def test_archive_names_layout_and_content(self):
        assets = package_release.package_release(
            self.dll, self.so, self.wheel, self.source, self.output, self.pdb
        )
        self.assertEqual(
            [asset.name for asset in assets],
            [
                package_release.WINDOWS_ARCHIVE_NAME,
                package_release.LINUX_ARCHIVE_NAME,
                package_release.SDK_ARCHIVE_NAME,
            ],
        )
        with zipfile.ZipFile(assets[0]) as archive:
            self.assertEqual(archive.namelist(), [self.dll.name, self.pdb.name])
            self.assertEqual(archive.read(self.dll.name), self.dll.read_bytes())
            self.assertEqual(archive.read(self.pdb.name), self.pdb.read_bytes())
        with tarfile.open(assets[1], "r:gz") as archive:
            self.assertEqual([member.name for member in archive.getmembers()], [self.so.name])
            self.assertEqual(archive.extractfile(self.so.name).read(), self.so.read_bytes())
        expected_sdk = {
            f"{package_release.SDK_ROOT}/{relative}"
            for relative in package_release.SDK_FILES
        }
        expected_sdk.add(f"{package_release.SDK_ROOT}/python/{self.wheel.name}")
        with zipfile.ZipFile(assets[2]) as archive:
            self.assertEqual(set(archive.namelist()), expected_sdk)
            for relative in package_release.SDK_FILES:
                self.assertEqual(
                    archive.read(f"{package_release.SDK_ROOT}/{relative}"),
                    (self.source / relative).read_bytes(),
                )
            self.assertEqual(
                archive.read(f"{package_release.SDK_ROOT}/python/{self.wheel.name}"),
                self.wheel.read_bytes(),
            )

    def test_archives_are_reproducible(self):
        first = package_release.package_release(
            self.dll, self.so, self.wheel, self.source, self.output, self.pdb
        )
        first_bytes = [asset.read_bytes() for asset in first]
        second = package_release.package_release(
            self.dll, self.so, self.wheel, self.source, self.output, self.pdb
        )
        self.assertEqual(first_bytes, [asset.read_bytes() for asset in second])

    def test_command_line_invocation(self):
        command = [
            sys.executable,
            str(SOURCE_ROOT / "tools" / "package_release.py"),
            "--windows-dll",
            str(self.dll),
            "--windows-pdb",
            str(self.pdb),
            "--linux-so",
            str(self.so),
            "--wheel",
            str(self.wheel),
            "--source-root",
            str(self.source),
            "--output-dir",
            str(self.output),
        ]
        result = subprocess.run(command, capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            sorted(path.name for path in self.output.iterdir()),
            sorted(
                (
                    package_release.WINDOWS_ARCHIVE_NAME,
                    package_release.LINUX_ARCHIVE_NAME,
                    package_release.SDK_ARCHIVE_NAME,
                )
            ),
        )

    def test_missing_or_wrong_wheel_fails(self):
        with self.assertRaises(ValueError):
            package_release.package_release(
                self.dll, self.so, self.root / "missing.whl", self.source, self.output
            )
        wrong_name = self.root / "other.whl"
        self._write_wheel(wrong_name)
        with self.assertRaises(ValueError):
            package_release.package_release(
                self.dll, self.so, wrong_name, self.source, self.output
            )
        self._write_wheel(self.wheel, metadata_version="0.2.0")
        with self.assertRaises(ValueError):
            package_release.package_release(
                self.dll, self.so, self.wheel, self.source, self.output
            )
        self._write_wheel(self.wheel, metadata_license="MIT")
        with self.assertRaises(ValueError):
            package_release.package_release(
                self.dll, self.so, self.wheel, self.source, self.output
            )

    def test_runtime_inputs_must_use_canonical_names(self):
        wrong_dll = self.root / "other.dll"
        wrong_dll.write_bytes(self.dll.read_bytes())
        with self.assertRaises(ValueError):
            package_release.package_release(
                wrong_dll, self.so, self.wheel, self.source, self.output
            )

        wrong_pdb = self.root / "other.pdb"
        wrong_pdb.write_bytes(self.pdb.read_bytes())
        with self.assertRaises(ValueError):
            package_release.package_release(
                self.dll,
                self.so,
                self.wheel,
                self.source,
                self.output,
                wrong_pdb,
            )

        wrong_so = self.root / "other.so"
        wrong_so.write_bytes(self.so.read_bytes())
        with self.assertRaises(ValueError):
            package_release.package_release(
                self.dll, wrong_so, self.wheel, self.source, self.output
            )


if __name__ == "__main__":
    unittest.main()
