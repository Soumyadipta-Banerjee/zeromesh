"""What runs on this machine: commands that cannot be split, and the final link."""

from __future__ import annotations

import os
import shutil
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from zm_cc.plan import Unit


@dataclass
class LocalResult:
    unit: Unit
    ok: bool
    seconds: float
    error: str = ""


def run(argv: Sequence[str], cwd: Path) -> tuple[int, str]:
    """Exit code and stderr of one command."""
    try:
        proc = subprocess.run(list(argv), cwd=str(cwd), capture_output=True, check=False)
    except OSError as e:
        return 127, str(e)
    return proc.returncode, proc.stderr.decode("utf-8", errors="replace")


def _tail(text: str, lines: int = 3) -> str:
    return " | ".join([ln for ln in text.strip().splitlines() if ln.strip()][-lines:])


def run_unit(u: Unit) -> LocalResult:
    """Runs a unit's command unchanged in its directory; an object it makes is moved to its place."""
    natural = u.natural_object() if u.object_path is not None else None
    before = natural.stat().st_mtime_ns if natural is not None and natural.is_file() else None
    t0 = time.monotonic()
    code, err = run(u.local_argv(), u.directory)
    secs = time.monotonic() - t0
    if code != 0:
        return LocalResult(u, False, secs, f"exit {code}: {_tail(err)}")
    if natural is None:
        return LocalResult(u, True, secs)
    if not natural.is_file() or natural.stat().st_size == 0 or natural.stat().st_mtime_ns == before:
        return LocalResult(u, False, secs, "no object produced")
    if natural != u.object_path:
        u.object_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(natural), str(u.object_path))
    return LocalResult(u, True, secs)


def run_units(units: Sequence[Unit]) -> list[LocalResult]:
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        return list(pool.map(run_unit, units))


def link_driver(units: Sequence[Unit]) -> str:
    """The compiler that links them: a C++ driver when any unit is C++."""
    with_objects = [u for u in units if u.object_path is not None]
    cxx = [u for u in with_objects if u.source and u.source.lang and u.source.lang.family == "c++"]
    pick = cxx or with_objects
    return pick[0].driver_local if pick else "cc"


def link(driver: str, objects: Sequence[str], output: Path, flags: Sequence[str]) -> tuple[int, str]:
    return run([driver, *objects, *flags, "-o", str(output)], Path.cwd())
