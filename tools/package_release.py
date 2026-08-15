#!/usr/bin/env python3
"""Build the deterministic MediaPlayer runtime and SDK release archives."""

from __future__ import annotations

import argparse
import gzip
import io
import os
import tarfile
import tempfile
import zipfile
from pathlib import Path


WHEEL_NAME = "endstone_mediaplayer-0.1.0-py3-none-any.whl"
WINDOWS_ARCHIVE_NAME = "endstone_mediaplayer-windows-x64.zip"
LINUX_ARCHIVE_NAME = "endstone_mediaplayer-linux-x64.tar.gz"
SDK_ARCHIVE_NAME = "endstone_mediaplayer-sdk.zip"
SDK_ROOT = "endstone_mediaplayer-sdk"
SDK_FILES = (
    "include/endstone_mediaplayer_api.h",
    "python/README.md",
    "examples/basic_screen.c",
    "examples/basic_screen.py",
    "docs/sdk.md",
    "LICENSE",
)


def _file(path: Path, description: str) -> Path:
    if not path.is_file():
        raise ValueError(f"missing {description}: {path}")
    return path


def _runtime_file(path: Path, expected_name: str, description: str) -> Path:
    path = _file(path, description)
    if path.name != expected_name:
        raise ValueError(f"{description} must be named {expected_name}: {path.name}")
    return path


def _validate_wheel(path: Path) -> Path:
    path = _file(path, "Python wheel")
    if path.name != WHEEL_NAME:
        raise ValueError(f"wheel must be named {WHEEL_NAME}: {path.name}")
    if not zipfile.is_zipfile(path):
        raise ValueError(f"wheel is not a valid ZIP archive: {path}")

    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        metadata_names = [
            name for name in names if name.endswith(".dist-info/METADATA")
        ]
        wheel_names = [name for name in names if name.endswith(".dist-info/WHEEL")]
        if len(metadata_names) != 1 or len(wheel_names) != 1:
            raise ValueError("wheel must contain exactly one dist-info METADATA and WHEEL")
        metadata = archive.read(metadata_names[0]).decode("utf-8", "replace")
        fields = {}
        for line in metadata.splitlines():
            key, separator, value = line.partition(":")
            if separator and key in {
                "Name",
                "Version",
                "License",
                "License-Expression",
            }:
                fields[key] = value.strip()
        license_value = fields.get("License") or fields.get("License-Expression")
        if (
            fields.get("Name") != "endstone-mediaplayer"
            or fields.get("Version") != "0.1.0"
            or license_value != "GPL-3.0-only"
        ):
            raise ValueError(
                "wheel metadata must identify endstone-mediaplayer 0.1.0 under GPL-3.0-only"
            )
        if "endstone_mediaplayer/__init__.py" not in names:
            raise ValueError("wheel does not contain the endstone_mediaplayer package")
    return path


def _archive_name(name: str) -> str:
    normalized = name.replace("\\", "/")
    if not normalized or normalized.startswith("/") or ".." in normalized.split("/"):
        raise ValueError(f"invalid archive member name: {name}")
    return normalized


def _write_zip(path: Path, entries: dict[str, bytes]) -> None:
    with zipfile.ZipFile(
        path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for name in sorted(entries):
            info = zipfile.ZipInfo(_archive_name(name), date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o644 << 16
            archive.writestr(info, entries[name])


def _write_tar_gz(path: Path, entries: dict[str, bytes]) -> None:
    with (
        path.open("wb") as output,
        gzip.GzipFile(fileobj=output, mode="wb", filename="", mtime=0) as compressed,
        tarfile.open(fileobj=compressed, mode="w", format=tarfile.GNU_FORMAT) as archive,
    ):
        for name in sorted(entries):
            data = entries[name]
            info = tarfile.TarInfo(_archive_name(name))
            info.size = len(data)
            info.mode = 0o644
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            archive.addfile(info, io.BytesIO(data))


def _validate_zip(path: Path, expected: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path) as archive:
        actual_names = archive.namelist()
        if actual_names != sorted(expected):
            raise ValueError(f"unexpected ZIP layout in {path.name}: {actual_names}")
        for name, data in expected.items():
            if archive.read(name) != data:
                raise ValueError(f"archive content mismatch for {name}")


def _validate_tar(path: Path, expected: dict[str, bytes]) -> None:
    with tarfile.open(path, mode="r:gz") as archive:
        members = archive.getmembers()
        actual_names = [member.name for member in members]
        if actual_names != sorted(expected):
            raise ValueError(f"unexpected TAR layout in {path.name}: {actual_names}")
        for name, data in expected.items():
            extracted = archive.extractfile(name)
            if extracted is None or extracted.read() != data:
                raise ValueError(f"archive content mismatch for {name}")


def _atomic_archive(
    destination: Path,
    writer: object,
    validator: object,
    expected: dict[str, bytes],
) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", dir=destination.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        writer(temporary, expected)
        validator(temporary, expected)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def package_release(
    windows_dll: str | os.PathLike[str],
    linux_so: str | os.PathLike[str],
    wheel: str | os.PathLike[str],
    source_root: str | os.PathLike[str],
    output_dir: str | os.PathLike[str],
    windows_pdb: str | os.PathLike[str] | None = None,
) -> tuple[Path, Path, Path]:
    """Create and validate all three release assets.

    Runtime archives use canonical binary names. The SDK preserves source-tree
    paths below one canonical top-level folder.
    """

    dll = _runtime_file(
        Path(windows_dll), "endstone_mediaplayer.dll", "Windows DLL"
    )
    so = _runtime_file(Path(linux_so), "endstone_mediaplayer.so", "Linux shared object")
    pdb = None
    if windows_pdb is not None:
        pdb = _runtime_file(
            Path(windows_pdb), "endstone_mediaplayer.pdb", "Windows PDB"
        )
    wheel_path = _validate_wheel(Path(wheel))
    root = Path(source_root)
    if not root.is_dir():
        raise ValueError(f"missing source root: {source_root}")

    source_bytes: dict[str, bytes] = {}
    for relative in SDK_FILES:
        source_bytes[relative] = _file(root / relative, f"SDK file {relative}").read_bytes()
    wheel_bytes = wheel_path.read_bytes()

    windows_entries = {dll.name: dll.read_bytes()}
    if pdb is not None:
        windows_entries[pdb.name] = pdb.read_bytes()
    linux_entries = {so.name: so.read_bytes()}
    sdk_entries = {
        f"{SDK_ROOT}/{relative}": data for relative, data in source_bytes.items()
    }
    sdk_entries[f"{SDK_ROOT}/python/{WHEEL_NAME}"] = wheel_bytes

    output = Path(output_dir)
    windows_archive = output / WINDOWS_ARCHIVE_NAME
    linux_archive = output / LINUX_ARCHIVE_NAME
    sdk_archive = output / SDK_ARCHIVE_NAME
    _atomic_archive(windows_archive, _write_zip, _validate_zip, windows_entries)
    _atomic_archive(linux_archive, _write_tar_gz, _validate_tar, linux_entries)
    _atomic_archive(sdk_archive, _write_zip, _validate_zip, sdk_entries)
    return windows_archive, linux_archive, sdk_archive


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--windows-dll", required=True, type=Path)
    parser.add_argument("--windows-pdb", type=Path)
    parser.add_argument("--linux-so", required=True, type=Path)
    parser.add_argument("--wheel", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    return parser


def main() -> int:
    args = _parser().parse_args()
    package_release(
        windows_dll=args.windows_dll,
        windows_pdb=args.windows_pdb,
        linux_so=args.linux_so,
        wheel=args.wheel,
        source_root=args.source_root,
        output_dir=args.output_dir,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
