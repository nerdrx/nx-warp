"""CPU discipline for the quality harness.

Every external heavy process (ffmpeg, x264, x265, the codec CLIs) is launched
through :func:`wrap` so that it lands on the agreed core slice at idle priority
and never competes with the interactive session or the compositor.

The prefix is ``chrt -i 0 taskset -c <cpus> nice -n 19``.

Override the slice with the ``NXQ_CPUS`` environment variable (default
``28-31``) and the ffmpeg thread count with ``NXQ_THREADS`` (default 4).
Set ``NXQ_NO_CPU_LIMIT=1`` to disable the prefix entirely (useful on machines
that lack ``chrt``/``taskset`` such as CI containers).
"""

from __future__ import annotations

import os
import shutil
import subprocess
from typing import Sequence

DEFAULT_CPUS = "28-31"
DEFAULT_THREADS = 4


def cpus() -> str:
    return os.environ.get("NXQ_CPUS", DEFAULT_CPUS)


def threads() -> int:
    try:
        return max(1, int(os.environ.get("NXQ_THREADS", DEFAULT_THREADS)))
    except ValueError:
        return DEFAULT_THREADS


def limiting_enabled() -> bool:
    if os.environ.get("NXQ_NO_CPU_LIMIT"):
        return False
    return shutil.which("chrt") is not None and shutil.which("taskset") is not None


def prefix() -> list[str]:
    """The ``chrt -i 0 taskset -c ... nice -n 19`` prefix, or ``[]``."""
    if not limiting_enabled():
        return []
    pre = ["chrt", "-i", "0", "taskset", "-c", cpus()]
    if shutil.which("nice"):
        pre += ["nice", "-n", "19"]
    return pre


def wrap(cmd: Sequence[str]) -> list[str]:
    """Return *cmd* prefixed with the CPU-discipline launcher."""
    return prefix() + list(cmd)


def run(
    cmd: Sequence[str],
    *,
    check: bool = True,
    capture: bool = True,
    timeout: float | None = None,
    cwd: str | None = None,
) -> subprocess.CompletedProcess:
    """Run *cmd* under the CPU discipline prefix."""
    full = wrap(cmd)
    return subprocess.run(
        full,
        check=check,
        capture_output=capture,
        text=True,
        timeout=timeout,
        cwd=cwd,
    )
