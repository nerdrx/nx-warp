"""pytest configuration: make ``tools/quality`` importable and mark slow tests."""

from __future__ import annotations

import os
import sys

import pytest

QUALITY_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if QUALITY_DIR not in sys.path:
    sys.path.insert(0, QUALITY_DIR)


def pytest_configure(config):
    config.addinivalue_line("markers", "slow: end-to-end tests that shell out to ffmpeg")


@pytest.fixture(scope="session")
def quality_dir() -> str:
    return QUALITY_DIR
