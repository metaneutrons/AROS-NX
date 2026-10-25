"""Keep the actual cross-target board-profile regression in Source CI."""

import importlib.util
from pathlib import Path


def load_tests(loader, tests, pattern):
    source = Path(__file__).resolve().parents[2]
    path = source / "arch/riscv-esp32p4/tools/tests/test_build_profile_scope.py"
    spec = importlib.util.spec_from_file_location("p4_build_profile_scope", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return loader.loadTestsFromModule(module)
