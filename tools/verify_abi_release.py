#!/usr/bin/env python3
"""Resolve and verify the exact runtime ABI artifact required by a release."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any


ABI_REPOSITORY = "ReallocAll/endstone-mediaplayer-abi"
ABI_WORKFLOW = ".github/workflows/abi.yml"
ABI_ARTIFACT = "endstone-mediaplayer-abi"
ENDSTONE_COMMIT = "94457f642426c957dba1ef7d09375398bc7f8279"
ENDSTONE_VERSION = "0.11.8"
BDS_VERSION = "26.40"
MEDIA_PLAYER_REPOSITORY = "ReallocAll/endstone-mediaplayer"
PLATFORMS = ("windows", "linux")
HEX_COMMIT = re.compile(r"^[0-9a-f]{40}$")


class VerificationError(ValueError):
    """An ABI proof or GitHub artifact lookup failed closed."""


def _read_json(path: Path, label: str) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise VerificationError(f"{label} is missing or malformed: {exc}") from exc


def _dict(value: Any, label: str, errors: list[str]) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.append(f"{label} must be an object")
        return {}
    return value


def _require(errors: list[str], condition: bool, message: str) -> None:
    if not condition:
        errors.append(message)


def _artifact_root(download_dir: Path) -> Path:
    """Find the final-artifact root in a gh download directory."""
    direct = download_dir / "abi-evidence"
    if direct.is_dir():
        return download_dir
    candidates = [
        child for child in download_dir.iterdir()
        if child.is_dir() and (child / "abi-evidence").is_dir()
    ]
    if len(candidates) != 1:
        raise VerificationError(
            "downloaded ABI artifact has no unique root containing abi-evidence"
        )
    return candidates[0]


def _sha256(path: Path) -> str:
    import hashlib

    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _platform_invariant(manifest: dict[str, Any], platform: str) -> dict[str, Any]:
    invariants = manifest.get("invariants")
    if not isinstance(invariants, dict):
        return {}
    value = invariants.get("description_layout")
    if not isinstance(value, dict):
        return {}
    if "status" in value:
        return value
    platform_value = value.get(platform)
    return platform_value if isinstance(platform_value, dict) else {}


def validate_artifact(
    root: Path, media_player_commit: str, expected_abi_run_id: str | None = None
) -> list[str]:
    """Return all release-gate failures for an extracted ABI artifact."""
    errors: list[str] = []
    evidence = root / "abi-evidence"
    required_files = {
        "manifest": evidence / "manifest.json",
        "coverage": evidence / "coverage.json",
        "requirements": evidence / "requirements.json",
        "windows runtime": evidence / "windows-runtime.json",
        "linux runtime": evidence / "linux-runtime.json",
        "windows consumer runtime": evidence / "windows-consumer-runtime.json",
        "linux consumer runtime": evidence / "linux-consumer-runtime.json",
    }
    loaded: dict[str, Any] = {}
    for label, path in required_files.items():
        if not path.is_file():
            errors.append(f"missing {label}: {path.relative_to(root).as_posix()}")
            loaded[label] = {}
            continue
        try:
            loaded[label] = _read_json(path, label)
        except VerificationError as exc:
            errors.append(str(exc))
            loaded[label] = {}

    manifest = _dict(loaded["manifest"], "manifest", errors)
    coverage = _dict(loaded["coverage"], "coverage", errors)
    requirements = _dict(loaded["requirements"], "requirements", errors)
    _require(errors, manifest.get("schema_version") == 1, "manifest schema_version must be 1")
    _require(errors, coverage.get("schema_version") == 1, "coverage schema_version must be 1")
    _require(errors, coverage.get("complete") is True, "coverage complete must be true")

    _require(
        errors,
        isinstance(media_player_commit, str) and bool(HEX_COMMIT.fullmatch(media_player_commit)),
        "current MediaPlayer commit must be a 40-character hexadecimal commit",
    )
    media = _dict(manifest.get("mediaplayer"), "manifest.mediaplayer", errors)
    _require(
        errors,
        media.get("repository") == MEDIA_PLAYER_REPOSITORY,
        "manifest MediaPlayer repository is not ReallocAll/endstone-mediaplayer",
    )
    for label, value in (
        ("manifest", media.get("commit")),
        ("requirements", requirements.get("source_commit")),
    ):
        _require(errors, value == media_player_commit, f"{label} MediaPlayer commit differs from current SHA")
    _require(
        errors,
        requirements.get("source_repository") == MEDIA_PLAYER_REPOSITORY,
        "requirements MediaPlayer repository is not ReallocAll/endstone-mediaplayer",
    )
    fingerprint = media.get("fingerprint")
    _require(errors, isinstance(fingerprint, str) and bool(fingerprint), "MediaPlayer fingerprint is missing")
    _require(errors, requirements.get("source_fingerprint") == fingerprint, "requirements fingerprint differs from manifest")
    _require(errors, manifest.get("source_fingerprint") == fingerprint, "manifest fingerprint differs from MediaPlayer identity")

    workflow = _dict(manifest.get("workflow"), "manifest.workflow", errors)
    _require(errors, isinstance(workflow.get("commit"), str) and bool(workflow.get("commit")), "manifest ABI workflow commit is missing")
    workflow_run_id = workflow.get("run_id")
    _require(errors, isinstance(workflow_run_id, (str, int)) and str(workflow_run_id).isdigit(), "manifest ABI workflow run_id is missing or non-numeric")
    if expected_abi_run_id is not None:
        _require(errors, str(workflow_run_id) == str(expected_abi_run_id), "manifest ABI workflow run_id differs from selected run")

    header_paths = _dict(manifest.get("header_paths"), "manifest.header_paths", errors)
    expected_headers = {
        "windows": "include/abi/windows_x86_64.h",
        "linux": "include/abi/linux_x86_64.h",
    }
    header_hashes = _dict(manifest.get("header_hashes"), "manifest.header_hashes", errors)
    for platform, relative in expected_headers.items():
        _require(errors, header_paths.get(platform) == [relative], f"manifest {platform} header path is not exact")
        target = root / relative
        _require(errors, target.is_file(), f"missing generated {platform} ABI header")
        expected_hash = header_hashes.get(relative)
        _require(errors, isinstance(expected_hash, str), f"manifest hash is missing for {relative}")
        if target.is_file() and isinstance(expected_hash, str):
            _require(errors, _sha256(target) == expected_hash, f"generated header hash mismatch for {relative}")

    platforms = _dict(manifest.get("platforms"), "manifest.platforms", errors)
    runtime_environments = _dict(manifest.get("runtime_environments"), "manifest.runtime_environments", errors)
    runtime_run_ids = _dict(manifest.get("runtime_run_ids"), "manifest.runtime_run_ids", errors)
    coverage_platforms = _dict(coverage.get("platforms"), "coverage.platforms", errors)
    invariants = manifest.get("invariants")
    _require(errors, isinstance(invariants, dict), "manifest invariant metadata is missing")

    expected_environment = {
        "endstone_runtime_version": ENDSTONE_VERSION,
        "source_ref": "v" + ENDSTONE_VERSION,
        "source_commit": ENDSTONE_COMMIT,
        "bds_version": BDS_VERSION,
    }
    for platform in PLATFORMS:
        section = _dict(platforms.get(platform), f"manifest.platforms.{platform}", errors)
        environment = _dict(section.get("environment"), f"manifest.platforms.{platform}.environment", errors)
        report = _dict(loaded[f"{platform} runtime"], f"{platform} runtime", errors)
        report_environment = _dict(report.get("environment"), f"{platform} runtime.environment", errors)
        for field, expected in expected_environment.items():
            _require(errors, environment.get(field) == expected, f"{platform} Endstone {field} identity mismatch")
            _require(errors, report_environment.get(field) == expected, f"{platform} runtime Endstone {field} identity mismatch")
        _require(errors, environment.get("platform") == platform, f"{platform} manifest environment platform mismatch")
        _require(errors, report_environment.get("platform") == platform, f"{platform} runtime environment platform mismatch")
        _require(errors, runtime_environments.get(platform) == environment, f"{platform} runtime environment is not embedded consistently")
        _require(errors, section.get("run_id") == report.get("run_id"), f"{platform} manifest runtime run_id mismatch")
        _require(errors, runtime_run_ids.get(platform) == report.get("run_id"), f"{platform} runtime run_id is not embedded consistently")

        coverage_section = _dict(coverage_platforms.get(platform), f"coverage.platforms.{platform}", errors)
        _require(errors, coverage_section.get("required") == coverage_section.get("resolved"), f"{platform} requirement coverage is not fully resolved")
        _require(errors, coverage_section.get("missing") == [], f"{platform} requirement coverage has missing entries")
        _require(errors, coverage_section.get("percent") == 100.0, f"{platform} requirement coverage is not 100%")
        _require(errors, coverage_section.get("environment") == environment, f"{platform} coverage environment mismatch")
        _require(errors, coverage_section.get("runtime_run_id") == report.get("run_id"), f"{platform} coverage runtime run_id mismatch")
        requirements_list = section.get("requirements")
        _require(errors, isinstance(requirements_list, list), f"{platform} manifest requirements are missing")
        if isinstance(requirements_list, list):
            _require(errors, len(requirements_list) == coverage_section.get("resolved"), f"{platform} manifest requirement count differs from coverage")

        invariant = _platform_invariant(manifest, platform)
        _require(errors, invariant.get("status") == "PASS", f"{platform} description-layout invariant did not PASS")
        for field in ("bounds", "equality", "alignment"):
            _require(errors, invariant.get(field) is True, f"{platform} description-layout invariant {field} check failed")

    consumer_validation = _dict(manifest.get("consumer_validation"), "manifest.consumer_validation", errors)
    consumer_runtime = _dict(manifest.get("consumer_runtime"), "manifest.consumer_runtime", errors)
    for platform in PLATFORMS:
        validation = _dict(consumer_validation.get(platform), f"consumer validation {platform}", errors)
        _require(errors, validation.get("schema_version") == 1, f"consumer validation schema_version is invalid for {platform}")
        _require(errors, validation.get("status") == "PASS", f"consumer validation did not PASS for {platform}")
        _require(errors, validation.get("platform") == platform, f"consumer validation platform mismatch for {platform}")
        _require(errors, validation.get("overlay_removed_existing_headers") is True, f"consumer overlay cleanup was not verified for {platform}")
        _require(errors, validation.get("source_commit") == media_player_commit, f"consumer validation MediaPlayer commit mismatch for {platform}")
        _require(errors, validation.get("source_fingerprint") == fingerprint, f"consumer validation fingerprint mismatch for {platform}")
        for phase in ("configure", "build", "ctest"):
            phase_value = _dict(validation.get(phase), f"consumer validation {platform}.{phase}", errors)
            _require(errors, phase_value.get("exit_code") == 0, f"consumer validation {phase} did not pass for {platform}")

        runtime = _dict(loaded[f"{platform} consumer runtime"], f"{platform} consumer runtime", errors)
        _require(errors, runtime.get("schema_version") == 1, f"consumer runtime schema_version is invalid for {platform}")
        _require(errors, runtime.get("status") == "PASS", f"consumer runtime did not PASS for {platform}")
        _require(errors, runtime.get("platform") == platform, f"consumer runtime platform mismatch for {platform}")
        for field, expected in (
            ("fresh_server", True),
            ("player_required", False),
            ("logical_screen_lifecycle", "PASS"),
            ("command_sender_message", "PASS"),
            ("graceful_shutdown", True),
            ("forced", False),
            ("exit_code", 0),
            ("media_player_commit", media_player_commit),
            ("endstone_commit", ENDSTONE_COMMIT),
            ("endstone_version", ENDSTONE_VERSION),
            ("bds_version", BDS_VERSION),
        ):
            _require(errors, runtime.get(field) == expected, f"consumer runtime {field} identity/result mismatch for {platform}")
        commands = runtime.get("commands")
        _require(errors, isinstance(commands, list) and {"mpm help", "mpv help"}.issubset(commands), f"consumer runtime commands are incomplete for {platform}")
        _require(errors, consumer_runtime.get(platform) == runtime, f"embedded consumer runtime differs for {platform}")

    return errors


def _gh_json(endpoint: str, *, paginate: bool = False) -> Any:
    command = ["gh", "api"]
    if paginate:
        command.extend(["--paginate", "--slurp"])
    command.extend(["--header", "Accept: application/vnd.github+json", endpoint])
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "unknown gh api failure"
        raise VerificationError(f"GitHub API request failed: {detail}")
    try:
        return json.loads(result.stdout)
    except ValueError as exc:
        raise VerificationError(f"GitHub API returned malformed JSON: {exc}") from exc


def _workflow_runs(repository: str) -> list[dict[str, Any]]:
    payload = _gh_json(
        f"repos/{repository}/actions/workflows/abi.yml/runs?status=success&per_page=100",
        paginate=True,
    )
    pages = payload if isinstance(payload, list) else [payload]
    runs: list[dict[str, Any]] = []
    for page in pages:
        if isinstance(page, dict) and isinstance(page.get("workflow_runs"), list):
            runs.extend(item for item in page["workflow_runs"] if isinstance(item, dict))
    return runs


def _run(repository: str, run_id: str) -> dict[str, Any]:
    payload = _gh_json(f"repos/{repository}/actions/runs/{run_id}")
    if not isinstance(payload, dict):
        raise VerificationError(f"ABI run {run_id} metadata is malformed")
    return payload


def _artifact_metadata(repository: str, run_id: str) -> dict[str, Any]:
    payload = _gh_json(f"repos/{repository}/actions/runs/{run_id}/artifacts?per_page=100")
    artifacts = payload.get("artifacts") if isinstance(payload, dict) else None
    if not isinstance(artifacts, list):
        raise VerificationError(f"ABI run {run_id} artifact listing is malformed")
    matches = [item for item in artifacts if isinstance(item, dict) and item.get("name") == ABI_ARTIFACT and item.get("expired") is not True]
    if len(matches) != 1:
        raise VerificationError(f"ABI run {run_id} has {len(matches)} usable artifacts named {ABI_ARTIFACT!r}; expected exactly one")
    return matches[0]


def _download_artifact(artifact: dict[str, Any], destination: Path) -> Path:
    url = artifact.get("archive_download_url")
    if not isinstance(url, str) or not url:
        raise VerificationError("ABI artifact archive URL is missing")
    result = subprocess.run(
        ["gh", "api", "--header", "Accept: application/vnd.github+json", url],
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        detail = result.stderr.decode(errors="replace").strip() or "unknown gh artifact download failure"
        raise VerificationError(f"ABI artifact download failed: {detail}")
    archive_path = destination / "artifact.zip"
    archive_path.write_bytes(result.stdout)
    try:
        with zipfile.ZipFile(archive_path) as archive:
            for member in archive.infolist():
                member_path = (destination / "extracted" / member.filename).resolve()
                if not str(member_path).startswith(str((destination / "extracted").resolve()) + os.sep):
                    raise VerificationError("ABI artifact contains an unsafe archive path")
            archive.extractall(destination / "extracted")
    except (OSError, zipfile.BadZipFile) as exc:
        raise VerificationError(f"ABI artifact archive is malformed: {exc}") from exc
    return _artifact_root(destination / "extracted")


def _manifest_commit(root: Path) -> str | None:
    try:
        value = _read_json(root / "abi-evidence" / "manifest.json", "ABI manifest")
    except VerificationError:
        return None
    if not isinstance(value, dict):
        return None
    media = value.get("mediaplayer")
    return media.get("commit") if isinstance(media, dict) and isinstance(media.get("commit"), str) else None


def _select_artifact(repository: str, media_player_commit: str, abi_run_id: str | None, work: Path) -> tuple[Path, str]:
    if abi_run_id is not None:
        run_ids = [abi_run_id]
    else:
        runs = _workflow_runs(repository)
        run_ids = [
            str(item.get("id"))
            for item in runs
            if item.get("status") == "completed"
            and item.get("conclusion") == "success"
            and item.get("path") == ABI_WORKFLOW
            and str(item.get("id", "")).isdigit()
        ]
    matches: list[tuple[Path, str]] = []
    for run_id in run_ids:
        run = _run(repository, run_id)
        if run.get("path") != ABI_WORKFLOW or run.get("status") != "completed" or run.get("conclusion") != "success":
            if abi_run_id is not None:
                raise VerificationError(f"ABI run {run_id} is not a completed successful {ABI_WORKFLOW} run")
            continue
        try:
            artifact = _artifact_metadata(repository, run_id)
        except VerificationError:
            if abi_run_id is not None:
                raise
            # Historical successful runs may have expired or predate the
            # production artifact. They cannot prove the current checkout,
            # so continue searching rather than treating one as a match.
            continue
        candidate_dir = work / f"run-{run_id}"
        candidate_dir.mkdir()
        try:
            root = _download_artifact(artifact, candidate_dir)
        except VerificationError:
            if abi_run_id is not None:
                raise
            continue
        if _manifest_commit(root) == media_player_commit:
            matches.append((root, run_id))
    if len(matches) != 1:
        raise VerificationError(
            f"expected exactly one successful ABI artifact matching MediaPlayer {media_player_commit}; found {len(matches)}"
        )
    return matches[0]


def verify_release(
    media_player_commit: str,
    *,
    artifact_root: Path | None = None,
    repository: str = ABI_REPOSITORY,
    abi_run_id: str | None = None,
) -> list[str]:
    """Verify a local artifact or resolve one exact successful GitHub ABI run."""
    if not HEX_COMMIT.fullmatch(media_player_commit):
        return ["current MediaPlayer commit must be a 40-character hexadecimal commit"]
    if abi_run_id is not None and not re.fullmatch(r"[1-9][0-9]*", abi_run_id):
        return ["abi_run_id must be a positive numeric GitHub run ID"]
    if artifact_root is not None:
        return validate_artifact(artifact_root.resolve(), media_player_commit, abi_run_id)
    try:
        with tempfile.TemporaryDirectory(prefix="verify-abi-") as directory:
            root, selected_run_id = _select_artifact(repository, media_player_commit, abi_run_id, Path(directory))
            errors = validate_artifact(root, media_player_commit, selected_run_id)
            if errors:
                return errors
            print(f"verified ABI artifact {ABI_ARTIFACT!r} from {repository} run {selected_run_id}")
            return []
    except (OSError, VerificationError, subprocess.SubprocessError) as exc:
        return [str(exc)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--media-player-commit", required=True)
    parser.add_argument("--abi-run-id")
    parser.add_argument("--repository", default=ABI_REPOSITORY)
    parser.add_argument("--artifact-root", type=Path)
    args = parser.parse_args(argv)
    errors = verify_release(
        args.media_player_commit,
        artifact_root=args.artifact_root,
        repository=args.repository,
        abi_run_id=args.abi_run_id,
    )
    if errors:
        for error in errors:
            print(f"invalid: {error}", file=sys.stderr)
        return 1
    print("valid release ABI proof")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
