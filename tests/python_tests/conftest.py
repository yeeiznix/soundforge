# SoundForge G6 — pytest fixtures for the ctypes parity layer (PLAN_G6.md §3.3).
"""Shared fixtures for tests/python_tests.

The lazy-load contract applies to conftest itself (SEC-G6-08): nothing here
imports ``soundforge_py.engine`` or loads ``libsoundforge.so`` at module
import time — the suite stays green on a source-only checkout (no native
build). Only the package dir ``python/`` is put on ``sys.path`` (same
convention as test_schema_py.py); never ``tests/python_tests``.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Repo root, resolved file-relatively: tests/python_tests/conftest.py -> repo.
REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES_DIR = REPO_ROOT / "tests" / "fixtures"

if str(REPO_ROOT / "python") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "python"))


@pytest.fixture(scope="session")
def repo_root() -> Path:
    """Repo root (file-relative, working-directory independent)."""
    return REPO_ROOT


@pytest.fixture(scope="session")
def fixtures_dir() -> Path:
    """Shared fixture corpus (tests/fixtures)."""
    return FIXTURES_DIR


@pytest.fixture(scope="session")
def soundforge_lib():
    """Cached CDLL for ``libsoundforge.so`` (lazy: engine imported only here).

    Honors ``$SOUNDFORGE_LIB_PATH`` (absolute, ``realpath``-canonicalized,
    ``isfile``-checked — used to point the suite at
    ``native/build-asan/libsoundforge.so``, PLAN_G6.md §3.6); default is
    ``REPO_ROOT/native/build/libsoundforge.so``. The CDLL is cached by the
    engine module singleton, so this fixture is created exactly once.
    """
    from soundforge_py.engine import get_lib  # lazy — no import-time load

    return get_lib()