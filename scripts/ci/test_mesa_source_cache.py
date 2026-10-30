#!/usr/bin/env python3
"""Regression checks for the locked Mesa source input in product CI."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from locked_product_sources import export, load_contract, stage, verify_cache


ROOT = Path(__file__).resolve().parents[2]
PLAN = ROOT / "scripts/ci/product-sources.plan.json"
STAGING = ROOT / "scripts/ci/product-sources.staging.json"
MESA_ROLE = "product:mesa@26.2.5"
MESA_FILENAME = "mesa-26.2.5.tar.xz"
MESA_SHA256 = "794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5"
MESA_CANDIDATES = [
    "https://archive.mesa3d.org/mesa-26.2.5.tar.xz",
    "https://sources.buildroot.net/mesa3d/mesa-26.2.5.tar.xz",
]
MESA_SOURCE_GUARD = (
    "MESA_ARCHIVE_CHECKSUMS := "
    f"{MESA_FILENAME}=sha256:{MESA_SHA256}"
)
ZLIB_ROLE = "product:chromium-zlib@2b1b62fed03e8b8a64ea39a96d5cc74cd10e2429"
ZLIB_CANDIDATES = [
    "https://raw.githubusercontent.com/metaneutrons/aros-toolchains/f35ecea2918c8c8314bf6878b4401af6ae45132d/toolchains/source-seeds/chromium-zlib-da752eb2.tar.gz",
    "https://chromium.googlesource.com/chromium/src/+archive/2b1b62fed03e8b8a64ea39a96d5cc74cd10e2429/third_party/zlib.tar.gz",
    "https://chromium.googlesource.com/chromium/src.git/+archive/2b1b62fed03e8b8a64ea39a96d5cc74cd10e2429/third_party/zlib.tar.gz",
]


class MesaSourceCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="aros-mesa-source-cache-")
        self.addCleanup(temporary.cleanup)
        self.temp = Path(temporary.name)

    def read_contract_data(self) -> tuple[dict[str, object], dict[str, object]]:
        return (
            json.loads(PLAN.read_text(encoding="utf-8")),
            json.loads(STAGING.read_text(encoding="utf-8")),
        )

    def write_contract_data(
        self, plan: dict[str, object], staging: dict[str, object]
    ) -> tuple[Path, Path]:
        plan_path = self.temp / "plan.json"
        staging_path = self.temp / "staging.json"
        plan_path.write_text(json.dumps(plan), encoding="utf-8")
        staging_path.write_text(json.dumps(staging), encoding="utf-8")
        return plan_path, staging_path

    def assert_repository_contract(self, plan_path: Path, staging_path: Path) -> None:
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        staging = json.loads(staging_path.read_text(encoding="utf-8"))
        plan_entries = plan["entries"]
        staging_entries = staging["entries"]
        mesa_sources = [entry for entry in plan_entries if entry["role"] == MESA_ROLE]
        mesa_staging = [entry for entry in staging_entries if entry["role"] == MESA_ROLE]
        self.assertEqual(len(mesa_sources), 1)
        self.assertEqual(len(mesa_staging), 1)

        mesa = mesa_sources[0]
        self.assertEqual(mesa["filename"], MESA_FILENAME)
        self.assertEqual(mesa["candidates"], [{"url": url} for url in MESA_CANDIDATES])
        self.assertEqual(mesa["representation"], "archive")
        self.assertEqual(mesa["normalization"], "exact-bytes-v1")
        self.assertEqual(
            mesa["integrity"],
            {"kind": "locked", "sha256": MESA_SHA256, "size": 68652756},
        )

        mapping = mesa_staging[0]
        self.assertEqual(mapping["destination"], "portssources/mesa-26.2.5.tar.xz")
        self.assertEqual(
            mapping["source_guard"],
            {"path": "workbench/libs/mesa/mesa.cfg", "line": MESA_SOURCE_GUARD},
        )
        mesa_cfg = (ROOT / "workbench/libs/mesa/mesa.cfg").read_text(encoding="utf-8")
        self.assertIn("OPT_MESAGL=26.2.5", mesa_cfg.splitlines())
        self.assertIn(MESA_SOURCE_GUARD, mesa_cfg.splitlines())
        mesautil_inventory = (
            ROOT / "workbench/libs/mesa/libmesautil/mesautil-26.2.5.sources"
        ).read_text(encoding="utf-8")
        self.assertNotIn("mesa-sha1.c", mesautil_inventory.split())

        # Pin the existing Zlib source contract so adding Mesa cannot silently
        # replace or weaken the source that was already shared by product CI.
        zlib = [entry for entry in plan_entries if entry["role"] == ZLIB_ROLE]
        zlib_staging = [entry for entry in staging_entries if entry["role"] == ZLIB_ROLE]
        self.assertEqual(len(zlib), 1)
        self.assertEqual(
            zlib[0],
            {
                "role": ZLIB_ROLE,
                "filename": "zlib.tar.gz",
                "candidates": [{"url": url} for url in ZLIB_CANDIDATES],
                "representation": "archive",
                "normalization": "canonical-tar-gzip-v1",
                "integrity": {
                    "kind": "locked",
                    "sha256": "3d56d66841a3380102abc6883fac7f756658417a9e6bc64af32a25aa57c3aeb6",
                    "size": 3261973,
                },
            },
        )
        self.assertEqual(len(zlib_staging), 1)
        self.assertEqual(
            zlib_staging[0],
            {
                "role": ZLIB_ROLE,
                "destination": "portssources/chromium-2b1b62fed03e8b8a64ea39a96d5cc74cd10e2429/zlib.tar.gz",
                "source_guard": {
                    "path": "workbench/libs/z/make.opt",
                    "line": "ZLIB_CHROMIUM_REVISION := 2b1b62fed03e8b8a64ea39a96d5cc74cd10e2429",
                },
            },
        )
        load_contract(plan_path, staging_path, ROOT)

    def test_repository_contract_locks_mesa_and_preserves_zlib(self) -> None:
        self.assert_repository_contract(PLAN, STAGING)

    def assert_galliumaux_llvm_header_contract(self, source: str) -> None:
        llvm_block = source.split("ifeq ($(AROS_TOOLCHAIN),llvm)", 1)[1].split("endif", 3)
        self.assertGreaterEqual(len(llvm_block), 4)
        selected_block = "endif".join(llvm_block[:4])
        self.assertIn("OPT_LLVM := $(TARGET_LLVM_VER)", selected_block)
        self.assertIn("OPT_LLVM := 11.0.0", selected_block)
        self.assertIn(
            "mesa3d-linklib-galliumauxiliary_MC_EXTRA_CPPFLAGS :=",
            selected_block,
        )
        self.assertIn(
            "-I$(PORTSDIR)/llvm/llvm-$(OPT_LLVM).src/include",
            selected_block,
        )
        self.assertIn(
            "gallivm/lp_bld_nir_no_integer_algebraic.c",
            selected_block,
        )
        self.assertIn(
            "#MM- mesa3d-linklib-galliumauxiliary-llvm : workbench-libs-llvm",
            source,
        )

    def test_galliumaux_llvm_generated_source_has_llvm_headers(self) -> None:
        source = (
            ROOT / "workbench/libs/mesa/libgalliumaux/mmakefile.src"
        ).read_text(encoding="utf-8")
        self.assert_galliumaux_llvm_header_contract(source)
        with self.assertRaises(AssertionError):
            self.assert_galliumaux_llvm_header_contract(
                source.replace(
                    "-I$(PORTSDIR)/llvm/llvm-$(OPT_LLVM).src/include",
                    "-I$(PORTSDIR)/llvm/missing/include",
                    1,
                )
            )

    def test_repository_contract_rejects_removed_mesa(self) -> None:
        plan, staging = self.read_contract_data()
        plan["entries"] = [entry for entry in plan["entries"] if entry["role"] != MESA_ROLE]
        staging["entries"] = [entry for entry in staging["entries"] if entry["role"] != MESA_ROLE]
        plan_path, staging_path = self.write_contract_data(plan, staging)
        with self.assertRaises(AssertionError):
            self.assert_repository_contract(plan_path, staging_path)

    def test_repository_contract_rejects_misbound_mesa_destination(self) -> None:
        plan, staging = self.read_contract_data()
        next(entry for entry in staging["entries"] if entry["role"] == MESA_ROLE)["destination"] = (
            "portssources/alternate/mesa-26.2.5.tar.xz"
        )
        plan_path, staging_path = self.write_contract_data(plan, staging)
        with self.assertRaises(AssertionError):
            self.assert_repository_contract(plan_path, staging_path)

    def test_repository_contract_rejects_wrong_mesa_source_guard(self) -> None:
        plan, staging = self.read_contract_data()
        next(entry for entry in staging["entries"] if entry["role"] == MESA_ROLE)["source_guard"]["line"] = (
            "MESA_ARCHIVE_CHECKSUMS := mesa-26.2.5.tar.xz=sha256:" + "0" * 64
        )
        plan_path, staging_path = self.write_contract_data(plan, staging)
        with self.assertRaisesRegex(ValueError, "source revision no longer matches"):
            load_contract(plan_path, staging_path, ROOT)

    def test_repository_contract_rejects_modified_mesa_digest(self) -> None:
        plan, staging = self.read_contract_data()
        next(entry for entry in plan["entries"] if entry["role"] == MESA_ROLE)["integrity"]["sha256"] = "0" * 64
        plan_path, staging_path = self.write_contract_data(plan, staging)
        with self.assertRaises(AssertionError):
            self.assert_repository_contract(plan_path, staging_path)

    def test_repository_contract_rejects_removed_mesa_fallback(self) -> None:
        plan, staging = self.read_contract_data()
        next(entry for entry in plan["entries"] if entry["role"] == MESA_ROLE)["candidates"] = [
            {"url": MESA_CANDIDATES[0]}
        ]
        plan_path, staging_path = self.write_contract_data(plan, staging)
        with self.assertRaises(AssertionError):
            self.assert_repository_contract(plan_path, staging_path)

    def test_repository_contract_rejects_reordered_mesa_fallback(self) -> None:
        plan, staging = self.read_contract_data()
        next(entry for entry in plan["entries"] if entry["role"] == MESA_ROLE)["candidates"] = [
            {"url": url} for url in reversed(MESA_CANDIDATES)
        ]
        plan_path, staging_path = self.write_contract_data(plan, staging)
        with self.assertRaises(AssertionError):
            self.assert_repository_contract(plan_path, staging_path)

    def test_repository_contract_rejects_unknown_mesa_mirror(self) -> None:
        plan, staging = self.read_contract_data()
        next(entry for entry in plan["entries"] if entry["role"] == MESA_ROLE)["candidates"][1] = {
            "url": "https://mirror.example.invalid/mesa-26.2.5.tar.xz"
        }
        plan_path, staging_path = self.write_contract_data(plan, staging)
        with self.assertRaises(AssertionError):
            self.assert_repository_contract(plan_path, staging_path)

    def synthetic_inputs(self) -> tuple[Path, list[dict[str, object]], dict[str, bytes]]:
        source_root = self.temp / "synthetic-source"
        source_root.mkdir()
        (source_root / "mesa.cfg").write_text("MESA_TEST_GUARD := mesa\n", encoding="utf-8")
        (source_root / "zlib.cfg").write_text("ZLIB_TEST_GUARD := zlib\n", encoding="utf-8")
        payloads = {
            MESA_FILENAME: b"tiny synthetic Mesa bytes\n",
            "zlib.tar.gz": b"tiny synthetic Zlib bytes\n",
        }
        cache = self.temp / "cache"
        cache.mkdir()
        plan_entries = []
        staging_entries = []
        specs = [
            (MESA_ROLE, MESA_FILENAME, "portssources/mesa-26.2.5.tar.xz", "mesa.cfg", "MESA_TEST_GUARD := mesa"),
            (ZLIB_ROLE, "zlib.tar.gz", "portssources/chromium-2b1b62fed03e8b8a64ea39a96d5cc74cd10e2429/zlib.tar.gz", "zlib.cfg", "ZLIB_TEST_GUARD := zlib"),
        ]
        for role, filename, destination, guard_path, guard_line in specs:
            payload = payloads[filename]
            (cache / filename).write_bytes(payload)
            plan_entries.append({
                "role": role,
                "filename": filename,
                "candidates": [{"url": "https://example.invalid/" + filename}],
                "representation": "archive",
                "normalization": "exact-bytes-v1",
                "integrity": {
                    "kind": "locked",
                    "sha256": hashlib.sha256(payload).hexdigest(),
                    "size": len(payload),
                },
            })
            staging_entries.append({
                "role": role,
                "destination": destination,
                "source_guard": {"path": guard_path, "line": guard_line},
            })
        plan_path, staging_path = self.write_contract_data(
            {"schema": "aros-cache-source-fetch-plan-v1", "entries": plan_entries},
            {"schema": "aros-nx-product-source-staging-v1", "entries": staging_entries},
        )
        entries = load_contract(plan_path, staging_path, source_root)
        return cache, entries, payloads

    def test_export_and_stage_copy_both_closed_cache_inputs_exactly(self) -> None:
        cache, entries, payloads = self.synthetic_inputs()
        verify_cache(entries, cache)
        exported = self.temp / "exported"
        export(entries, cache, exported)
        verify_cache(entries, exported)

        build_root = self.temp / "build"
        stage(entries, exported, build_root)
        self.assertFalse((build_root / "CMakeCache.txt").exists())
        for entry in entries:
            self.assertEqual(
                build_root.joinpath(*entry["destination"].parts).read_bytes(),
                payloads[entry["filename"]],
            )

    def test_missing_mesa_fails_before_build_root_creation(self) -> None:
        cache, entries, _ = self.synthetic_inputs()
        (cache / MESA_FILENAME).unlink()
        build_root = self.temp / "missing-build"
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            stage(entries, cache, build_root)
        self.assertFalse(build_root.exists())

    def test_corrupt_mesa_fails_before_build_root_creation(self) -> None:
        cache, entries, _ = self.synthetic_inputs()
        (cache / MESA_FILENAME).write_bytes(b"corrupt Mesa payload\n")
        build_root = self.temp / "corrupt-build"
        with self.assertRaisesRegex(ValueError, "integrity mismatch"):
            stage(entries, cache, build_root)
        self.assertFalse(build_root.exists())

    def test_extra_cache_artifact_is_rejected(self) -> None:
        cache, entries, _ = self.synthetic_inputs()
        (cache / "unexpected.bin").write_bytes(b"extra\n")
        with self.assertRaisesRegex(ValueError, "inventory mismatch"):
            verify_cache(entries, cache)

    def test_duplicate_source_role_is_rejected(self) -> None:
        plan, _ = self.read_contract_data()
        plan["entries"][1]["role"] = plan["entries"][0]["role"]
        plan_path, staging_path = self.write_contract_data(plan, json.loads(STAGING.read_text(encoding="utf-8")))
        with self.assertRaisesRegex(ValueError, "duplicate or invalid source role"):
            load_contract(plan_path, staging_path, ROOT)


if __name__ == "__main__":
    unittest.main()
