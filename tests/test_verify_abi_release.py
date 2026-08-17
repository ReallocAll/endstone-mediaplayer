from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from tools.verify_abi_release import ENDSTONE_COMMIT, ENDSTONE_VERSION, validate_artifact


MEDIA_COMMIT = "a" * 40
FINGERPRINT = "f" * 64


class VerifyAbiReleaseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        (self.root / "include/abi").mkdir(parents=True)
        (self.root / "abi-evidence").mkdir()
        (self.root / "include/abi/windows_x86_64.h").write_text("windows\n", encoding="utf-8")
        (self.root / "include/abi/linux_x86_64.h").write_text("linux\n", encoding="utf-8")
        self._write_fixture()

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def _write_fixture(self) -> None:
        environments = {
            platform: {
                "platform": platform,
                "source_ref": "v" + ENDSTONE_VERSION,
                "source_commit": ENDSTONE_COMMIT,
                "endstone_runtime_version": ENDSTONE_VERSION,
                "bds_version": "26.40",
            }
            for platform in ("windows", "linux")
        }
        runtimes = {
            platform: {"schema_version": 1, "run_id": f"123-{platform}", "environment": environment}
            for platform, environment in environments.items()
        }
        consumer_runtimes = {
            platform: {
                "schema_version": 1,
                "status": "PASS",
                "platform": platform,
                "fresh_server": True,
                "player_required": False,
                "logical_screen_lifecycle": "PASS",
                "command_sender_message": "PASS",
                "graceful_shutdown": True,
                "forced": False,
                "exit_code": 0,
                "media_player_commit": MEDIA_COMMIT,
                "endstone_commit": ENDSTONE_COMMIT,
                "endstone_version": ENDSTONE_VERSION,
                "bds_version": "26.40",
                "commands": ["mpm help", "mpv help"],
            }
            for platform in ("windows", "linux")
        }
        consumer_validation = {
            platform: {
                "schema_version": 1,
                "status": "PASS",
                "platform": platform,
                "overlay_removed_existing_headers": True,
                "source_commit": MEDIA_COMMIT,
                "source_fingerprint": FINGERPRINT,
                "configure": {"exit_code": 0},
                "build": {"exit_code": 0},
                "ctest": {"exit_code": 0},
            }
            for platform in ("windows", "linux")
        }
        manifest = {
            "schema_version": 1,
            "mediaplayer": {
                "repository": "ReallocAll/endstone-mediaplayer",
                "commit": MEDIA_COMMIT,
                "fingerprint": FINGERPRINT,
            },
            "source_fingerprint": FINGERPRINT,
            "header_paths": {
                "windows": ["include/abi/windows_x86_64.h"],
                "linux": ["include/abi/linux_x86_64.h"],
            },
            "header_hashes": {},
            "platforms": {},
            "runtime_environments": environments,
            "runtime_run_ids": {platform: value["run_id"] for platform, value in runtimes.items()},
            "invariants": {
                "description_layout": {
                    platform: {
                        "status": "PASS",
                        "bounds": True,
                        "equality": True,
                        "alignment": True,
                    }
                    for platform in ("windows", "linux")
                }
            },
            "consumer_validation": consumer_validation,
            "consumer_runtime": consumer_runtimes,
            "workflow": {"commit": "b" * 40, "run_id": "123"},
        }
        requirements = {
            "schema_version": 1,
            "source_repository": "ReallocAll/endstone-mediaplayer",
            "source_commit": MEDIA_COMMIT,
            "source_fingerprint": FINGERPRINT,
        }
        for platform in ("windows", "linux"):
            entry = {"name": f"ES_{platform.upper()}_SIZE"}
            manifest["platforms"][platform] = {
                "environment": environments[platform],
                "run_id": runtimes[platform]["run_id"],
                "requirements": [entry],
            }
        coverage = {
            "schema_version": 1,
            "complete": True,
            "platforms": {
                platform: {
                    "required": 1,
                    "resolved": 1,
                    "missing": [],
                    "percent": 100.0,
                    "environment": environments[platform],
                    "runtime_run_id": runtimes[platform]["run_id"],
                }
                for platform in ("windows", "linux")
            },
        }
        manifest["header_hashes"] = {
            relative: hashlib.sha256((self.root / relative).read_bytes()).hexdigest()
            for relative in ("include/abi/windows_x86_64.h", "include/abi/linux_x86_64.h")
        }
        evidence = self.root / "abi-evidence"
        for name, value in (
            ("manifest.json", manifest),
            ("coverage.json", coverage),
            ("requirements.json", requirements),
            ("windows-runtime.json", runtimes["windows"]),
            ("linux-runtime.json", runtimes["linux"]),
            ("windows-consumer-runtime.json", consumer_runtimes["windows"]),
            ("linux-consumer-runtime.json", consumer_runtimes["linux"]),
        ):
            (evidence / name).write_text(json.dumps(value), encoding="utf-8")

    def test_valid_artifact_passes(self) -> None:
        self.assertEqual(validate_artifact(self.root, MEDIA_COMMIT, "123"), [])

    def test_media_player_commit_mismatch_fails(self) -> None:
        errors = validate_artifact(self.root, "c" * 40, "123")
        self.assertTrue(any("commit differs" in error for error in errors))

    def test_missing_consumer_proof_fails(self) -> None:
        (self.root / "abi-evidence/linux-consumer-runtime.json").unlink()
        errors = validate_artifact(self.root, MEDIA_COMMIT, "123")
        self.assertTrue(any("missing linux consumer runtime" in error for error in errors))

    def test_endstone_identity_and_coverage_failures_are_reported(self) -> None:
        manifest_path = self.root / "abi-evidence/manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["platforms"]["windows"]["environment"]["bds_version"] = "26.41"
        manifest["invariants"]["description_layout"]["linux"]["status"] = "FAIL"
        coverage_path = self.root / "abi-evidence/coverage.json"
        coverage = json.loads(coverage_path.read_text(encoding="utf-8"))
        coverage["platforms"]["windows"]["percent"] = 99.0
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        coverage_path.write_text(json.dumps(coverage), encoding="utf-8")
        errors = validate_artifact(self.root, MEDIA_COMMIT, "123")
        self.assertTrue(any("bds_version identity mismatch" in error for error in errors))
        self.assertTrue(any("coverage is not 100%" in error for error in errors))
        self.assertTrue(any("invariant did not PASS" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
