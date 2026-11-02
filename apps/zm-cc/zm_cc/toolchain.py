"""The compiler as an environment: what a donor needs to compile these units."""

from __future__ import annotations

import os
import shlex
import subprocess
import tempfile
from pathlib import Path
from typing import Sequence

from zeromesh.adapter import Env, Program

from zm_cc.plan import Unit

_X = {"c": "c", "c++": "c++", "fortran": "f95"}


def host_env(units: Sequence[Unit]) -> Env:
    """Donors use their own compiler, which must be the exact build this machine has (the driver,
    the programs it runs and their libraries), so objects are the same wherever they compile."""
    return Env.host(*sorted({Program(u.driver_remote) for u in units}, key=lambda p: p.name))


def search_dirs(driver: str, family: str) -> list[str]:
    """The compiler's own header search list (`cc -E -v`), as existing real paths."""
    proc = subprocess.run([driver, "-E", "-v", "-x", _X[family], os.devnull], capture_output=True, text=True)
    dirs, inside = [], False
    for line in proc.stderr.splitlines():
        if line.startswith("#include <...> search starts here:"):
            inside = True
        elif line.startswith("End of search list."):
            break
        elif inside:
            d = os.path.realpath(line.strip().split(" (")[0])
            if os.path.isdir(d):
                dirs.append(d)
    return dirs


def capture_env(units: Sequence[Unit], workspace: Path) -> Env:
    """The compiler captured here: one real compile per (compiler, language) under a trace takes the
    driver, cc1/cc1plus/f951, the assembler and their libraries; the header search list is taken
    whole, because other units include headers this one does not."""
    probes: list[list[str]] = []
    paths: set[str] = set()
    seen: set[tuple[str, str]] = set()
    ws = os.path.realpath(workspace)
    for u in units:
        family = u.source.lang.family
        if (u.driver_local, family) in seen:
            continue
        seen.add((u.driver_local, family))
        # A stable path: the probe's command keys the capture, so an unchanged toolchain is reused.
        scratch = Path(tempfile.gettempdir()) / f"zm-cc-capture-{os.getuid()}"
        scratch.mkdir(mode=0o700, exist_ok=True)
        obj = str(scratch / f"{u.name}.o")
        argv = u.command.compile_argv(u.source, u.driver_local, obj)
        probes.append(["/bin/sh", "-c", f"cd {shlex.quote(str(u.directory))} && exec {shlex.join(argv)}"])
        paths.update(d for d in search_dirs(u.driver_local, family) if d != ws and not d.startswith(ws + os.sep))
    return Env.capture(probes, sorted(paths))
