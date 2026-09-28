#!/usr/bin/env python3
"""Positive and counter-probes for the product source staging contract."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from locked_product_sources import export, load_contract, stage, verify_cache


class LockedSourceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="aros-nx-sources-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source_root = self.root / "source"
        self.source_root.mkdir()
        (self.source_root / "make.opt").write_text("SOURCE_REVISION := abc123\n", encoding="utf-8")
        self.cache = self.root / "cache"
        self.cache.mkdir()
        self.payload = b"closed source input\n"
        (self.cache / "input.tar.gz").write_bytes(self.payload)
        self.plan = self.root / "plan.json"
        self.staging = self.root / "staging.json"
        self._write_plan()
        self._write_staging()

    def _write_plan(self, **entry_changes: object) -> None:
        entry = {
            "role": "product:test@abc123",
            "filename": "input.tar.gz",
            "candidates": [{"url": "https://example.invalid/input.tar.gz"}],
            "representation": "archive",
            "normalization": "canonical-tar-gzip-v1",
            "integrity": {
                "kind": "locked",
                "sha256": hashlib.sha256(self.payload).hexdigest(),
                "size": len(self.payload),
            },
        }
        entry.update(entry_changes)
        self.plan.write_text(
            json.dumps({"schema": "aros-cache-source-fetch-plan-v1", "entries": [entry]}),
            encoding="utf-8",
        )

    def _write_staging(self, **entry_changes: object) -> None:
        entry = {
            "role": "product:test@abc123",
            "destination": "portssources/test-abc123/input.tar.gz",
            "source_guard": {"path": "make.opt", "line": "SOURCE_REVISION := abc123"},
        }
        entry.update(entry_changes)
        self.staging.write_text(
            json.dumps({"schema": "aros-nx-product-source-staging-v1", "entries": [entry]}),
            encoding="utf-8",
        )

    def _entries(self) -> list[dict[str, object]]:
        return load_contract(self.plan, self.staging, self.source_root)

    def test_valid_input_stages_exact_bytes(self) -> None:
        entries = self._entries()
        verify_cache(entries, self.cache)
        build = self.root / "build"
        stage(entries, self.cache, build)
        self.assertEqual(
            (build / "portssources/test-abc123/input.tar.gz").read_bytes(), self.payload
        )

    def test_modified_cache_is_rejected(self) -> None:
        (self.cache / "input.tar.gz").write_bytes(b"modified source input\n")
        with self.assertRaisesRegex(ValueError, "integrity mismatch"):
            verify_cache(self._entries(), self.cache)

    def test_extra_cache_entry_is_rejected(self) -> None:
        (self.cache / "unexpected").write_bytes(b"extra")
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            verify_cache(self._entries(), self.cache)

    def test_internal_fetch_metadata_is_excluded_from_closed_export(self) -> None:
        (self.cache / ".aros-cache-lifecycle").write_text("metadata", encoding="utf-8")
        (self.cache / ".aros-fetch-example.lock").write_text("", encoding="utf-8")
        exported = self.root / "exported"
        export(self._entries(), self.cache, exported)
        self.assertEqual(sorted(item.name for item in exported.iterdir()), ["input.tar.gz"])
        verify_cache(self._entries(), exported)
        build = self.root / "build"
        stage(self._entries(), exported, build)
        self.assertEqual((build / "portssources/test-abc123/input.tar.gz").read_bytes(), self.payload)

    def test_modified_input_cannot_be_exported(self) -> None:
        (self.cache / "input.tar.gz").write_bytes(b"modified source input\n")
        with self.assertRaisesRegex(ValueError, "integrity mismatch"):
            export(self._entries(), self.cache, self.root / "exported")

    def test_unlocked_source_is_rejected(self) -> None:
        self._write_plan(integrity={"kind": "unverified", "max_size": 1024})
        with self.assertRaisesRegex(ValueError, "not locked"):
            self._entries()

    def test_revision_guard_rejects_source_drift(self) -> None:
        (self.source_root / "make.opt").write_text("SOURCE_REVISION := changed\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "revision no longer matches"):
            self._entries()

    def test_unsafe_destination_is_rejected(self) -> None:
        self._write_staging(destination="../outside/input.tar.gz")
        with self.assertRaisesRegex(ValueError, "unsafe staging destination"):
            self._entries()

    def test_existing_cmake_cache_is_rejected(self) -> None:
        build = self.root / "build"
        build.mkdir()
        (build / "CMakeCache.txt").write_text("configured\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "before CMake configures"):
            stage(self._entries(), self.cache, build)

    def test_symlinked_cache_entry_is_rejected(self) -> None:
        (self.cache / "input.tar.gz").unlink()
        (self.cache / "input.tar.gz").symlink_to(self.root / "elsewhere")
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            verify_cache(self._entries(), self.cache)


if __name__ == "__main__":
    unittest.main()
