"""CPU discipline for the hybrid simulator.

Every heavy external process (ffmpeg / x265) is launched through :func:`run`
so that it lands on the agreed core slice at idle priority and never competes
with the interactive session or the compositor.

Prefix: ``chrt -i 0 taskset -c <cpus> nice -n 19``.

Environment:

``NXVCH_CPUS``          core slice, default ``12-15``
``NXVCH_THREADS``       ffmpeg ``-threads`` / x265 ``pools``, default 4
``NXVCH_NO_CPU_LIMIT``  set to disable the prefix entirely

The slice is intersected with the CPUs this process may actually run on, so the
``12-15`` default pins on the development host and simply omits ``taskset`` on a
4-core CI runner rather than failing the whole run.
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


def _parse_cpu_spec(spec: str) -> set[int]:
    """Parse a ``taskset -c`` list (``"4"``, ``"4-7"``, ``"0,2,4-6"``) to a set."""
    out: set[int] = set()
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, _, hi = part.partition("-")
            try:
                out.update(range(int(lo), int(hi) + 1))
            except ValueError:
                return set()
        else:
            try:
                out.add(int(part))
            except ValueError:
                return set()
    return out


def _online_cpus() -> set[int]:
    """The CPUs this process is actually allowed to run on."""
    try:
        return set(os.sched_getaffinity(0))  # type: ignore[attr-defined]
    except (AttributeError, OSError):
        n = os.cpu_count() or 1
        return set(range(n))


def _compress(cpus_set: set[int]) -> str:
    """Render a set of CPU ids back into ``taskset -c`` range syntax."""
    ids = sorted(cpus_set)
    runs: list[str] = []
    start = prev = ids[0]
    for c in ids[1:]:
        if c == prev + 1:
            prev = c
            continue
        runs.append(str(start) if start == prev else f"{start}-{prev}")
        start = prev = c
    runs.append(str(start) if start == prev else f"{start}-{prev}")
    return ",".join(runs)


def usable_cpus() -> str | None:
    """The requested slice narrowed to CPUs that exist, or ``None``.

    A fixed slice like ``28-31`` is correct on the 32-core development host and
    nonsense on a 4-core CI runner, where ``taskset`` fails outright with
    "Invalid argument" and takes the whole test down with it.  Intersecting the
    request with the process's real affinity mask means the discipline still
    applies wherever it can, and simply does not pin where it cannot.
    """
    wanted = _parse_cpu_spec(cpus())
    if not wanted:
        return None
    online = _online_cpus()
    usable = wanted & online
    if not usable:
        return None
    return _compress(usable)


def limiting_enabled() -> bool:
    if os.environ.get("NXVCH_NO_CPU_LIMIT"):
        return False
    # Any one of the three is worth having; prefix() picks whichever exist.
    return any(shutil.which(t) for t in ("chrt", "taskset", "nice"))


def prefix() -> list[str]:
    """The ``chrt -i 0 taskset -c ... nice -n 19`` prefix, or ``[]``.

    ``taskset`` is dropped when none of the requested CPUs exist on this
    machine; the idle scheduling class and the nice level still apply.
    """
    if not limiting_enabled():
        return []
    pre: list[str] = []
    if shutil.which("chrt"):
        pre += ["chrt", "-i", "0"]
    slice_ = usable_cpus()
    if slice_ is not None and shutil.which("taskset"):
        pre += ["taskset", "-c", slice_]
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
