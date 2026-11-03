"""Link a meson/ninja build from objects that zm-cc compiled.

ninja will not adopt objects it did not build: with no .ninja_log entry and no depfile record it
calls them dirty and compiles them again. So the link steps are taken from ``ninja -t commands``,
the object paths in them are pointed at zm-cc's objects, and they run without ninja's compile step.
"""

from __future__ import annotations

import json
import shlex
import subprocess
from pathlib import Path
from typing import Sequence

from zm_cc.plan import Unit


def object_map(compdb: Path, compiled: Sequence[Unit]) -> dict[str, str]:
    """ninja's output path (relative to the build dir) -> the object zm-cc produced for it."""
    entries = json.loads(compdb.read_text())
    mapping: dict[str, str] = {}
    for u in compiled:
        out = entries[u.entry].get("output")
        if out and u.object_path is not None:
            mapping[out] = str(u.object_path)
    return mapping


def link_commands(build_dir: Path, targets: Sequence[str]) -> list[str]:
    """Every non-compile, non-codegen command needed to build ``targets``, in dependency order."""
    res = subprocess.run(
        ["ninja", "-C", str(build_dir), "-t", "commands", *targets],
        capture_output=True,
        text=True,
    )
    if res.returncode != 0:
        raise RuntimeError(f"ninja -t commands failed: {res.stderr.strip()[-400:]}")
    cmds = []
    for line in res.stdout.splitlines():
        if " -c " in line or line.startswith("/usr/bin/protoc"):
            continue
        cmds.append(line)
    return cmds


def rewrite(cmd: str, mapping: dict[str, str]) -> str:
    """Swap each ninja object path in ``cmd`` for the zm-cc object; whole shell words only."""
    words = shlex.split(cmd, posix=True) if "&&" not in cmd else None
    if words is not None:
        return shlex.join(mapping.get(w, w) for w in words)
    # compound command (e.g. "rm -f lib.a && gcc-ar ..."): rewrite each && part on its own
    return " && ".join(rewrite(part.strip(), mapping) for part in cmd.split("&&"))


def link_build(
    build_dir: Path, compdb: Path, compiled: Sequence[Unit], targets: Sequence[str]
) -> tuple[int, str, int]:
    """Run the link steps for ``targets``. Returns (exit code, error text, objects substituted)."""
    mapping = object_map(compdb, compiled)
    used = 0
    for cmd in link_commands(build_dir, targets):
        new = rewrite(cmd, mapping)
        used += sum(1 for w in shlex.split(cmd.replace("&&", " ")) if w in mapping)
        res = subprocess.run(["sh", "-c", new], cwd=build_dir, capture_output=True, text=True)
        if res.returncode != 0:
            return res.returncode, res.stderr, used
    return 0, "", used
