"""CPU discipline for the hybrid simulator.

Every heavy external process (ffmpeg / x265) is launched through :func:`run`
so that it lands on the agreed core slice at idle priority and never competes
with the interactive session or the compositor.

Prefix: ``chrt -i 0 taskset -c <cpus> nice -n 19``.

Environment:

``NXVCH_CPUS``          core slice, default ``12-15``
``NXVCH_THREADS``       ffmpeg ``-threads`` / x265 ``pools``, default 4
``NXVCH_NO_CPU_LIMIT``  set to disable the prefix (CI containers)
"""

from __future__ import annotations

import os
import shutil
import subprocess
from typing import Sequence

DEFAULT_CPUS = "12-15"
DEFAULT_THREADS = 4


def cpus() -> str:
    return os.environ.get("NXVCH_CPUS", DEFAULT_CPUS)


def threads() -> int:
    try:
        return max(1, int(os.environ.get("NXVCH_THREADS", DEFAULT_THREADS)))
    except ValueError:
        return DEFAULT_THREADS


def limiting_enabled() -> bool:
    if os.environ.get("NXVCH_NO_CPU_LIMIT"):
        return False
    return shutil.which("chrt") is not None and shutil.which("taskset") is not None


def prefix() -> list[str]:
    if not limiting_enabled():
        return []
    pre = ["chrt", "-i", "0", "taskset", "-c", cpus()]
    if shutil.which("nice"):
        pre += ["nice", "-n", "19"]
    return pre


def wrap(cmd: Sequence[str]) -> list[str]:
    return prefix() + list(cmd)


def run(
    cmd: Sequence[str],
    *,
    check: bool = True,
    capture: bool = True,
    timeout: float | None = None,
) -> subprocess.CompletedProcess:
    return subprocess.run(
        wrap(cmd), check=check, capture_output=capture, text=True, timeout=timeout
    )
