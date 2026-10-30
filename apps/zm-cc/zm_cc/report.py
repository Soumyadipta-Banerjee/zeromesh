"""Per-unit records, the zm-cc-report.json summary, and failure messages."""

from __future__ import annotations

import json
import re
import shlex
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Optional

from zm_cc.plan import Unit

# Donor refusals we know how to explain. First match wins.
_HINTS: tuple[tuple[str, str], ...] = (
    (
        r"owner busy|comfort level",
        "the donor's comfort governor is holding work. Set `max_step = 0` in donor.toml, "
        "or run the donor with ZMF_NO_GOVERNOR=1 (benchmarking only). See README troubleshooting.",
    ),
    (
        r"toolchain for .* differs",
        "env=host needs byte-identical compiler installs on every donor. Use --env mirror "
        "with a root made by `zm-cc mkroot`.",
    ),
)


@dataclass
class UnitRecord:
    index: int
    source: str
    mode: str  # "remote" or "local"
    command: str  # the command actually run, shell-quoted
    directory: str
    output: str = ""
    status: str = "pending"  # pending | ok | failed | skipped
    reason: str = ""  # why a unit is local
    node: str = ""  # "host (abcd1234)" for remote units
    node_id: str = ""
    task: str = ""
    wall_s: float = 0.0  # local units: time of the compile
    task_wall_s: float = 0.0  # remote units: time of the chunk task, shared by its units
    ship_bytes: int = 0  # bytes of preprocessed input sent to the donor
    cache_hit: bool = False
    error: str = ""

    @classmethod
    def for_unit(cls, u: Unit) -> "UnitRecord":
        cmd = u.remote_argv() if u.remote else u.local_argv()
        return cls(
            index=u.index,
            source=u.label,
            mode="remote" if u.remote else "local",
            command=shlex.join(cmd),
            directory=str(u.directory),
            output=str(u.object_path) if u.object_path is not None else "",
            reason=u.reason,
        )


def task_failure(res: Any) -> str:
    """Why a daemon task failed, or "" when it did not."""
    if res is None:
        return "no task result from daemon"
    if res.cancelled:
        return "cancelled"
    if res.timed_out:
        return "timed out"
    if not (res.exit_code or res.error):
        return ""
    if res.exit_code == 0:
        return res.error  # refused by the daemon or donor before the command ran
    err = (res.stderr_head or b"").decode("utf-8", errors="replace")
    lines = [ln for ln in err.splitlines() if ln.strip()]
    tail = " | ".join(lines[-3:])
    msg = f"exit {res.exit_code}: {tail}" if tail else f"exit code {res.exit_code}"
    if res.error:
        msg = f"{msg} ({res.error})"
    return msg


def explain(message: str) -> str:
    """Append a one-line hint when the message matches a known donor refusal."""
    for pattern, hint in _HINTS:
        if re.search(pattern, message):
            return f"{message}\n    hint: {hint}"
    return message


def peer_names(client: Any) -> dict[str, str]:
    """Map node_id (and its 8-char prefix) to hostname. Empty when the daemon cannot say."""
    out: dict[str, str] = {}
    try:
        for p in client.peers():
            nid = getattr(p, "node_id", "") or ""
            if not nid:
                continue
            host = getattr(p, "hostname", "") or nid[:8]
            out[nid] = host
            out[nid[:8]] = host
    except Exception:
        pass
    return out


def who(node_id: str, names: dict[str, str]) -> str:
    if not node_id:
        return "?"
    host = names.get(node_id) or names.get(node_id[:8])
    return f"{host} ({node_id[:8]})" if host else node_id[:8]


def write_report(path: Path, records: list[UnitRecord], *, total_s: float, meta: dict[str, Any]) -> None:
    """Write zm-cc-report.json: compile_commands-style units plus the object -> source manifest."""
    doc = {
        "tool": "zm-cc",
        **meta,
        "total_wall_s": round(total_s, 3),
        "failed": sum(1 for r in records if r.status == "failed"),
        "units": [asdict(r) for r in records],
        "objects": {r.output: r.source for r in records if r.status == "ok" and r.output},
    }
    path.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")


def who_did_what(records: list[UnitRecord]) -> list[tuple[str, list[str]]]:
    """Group unit labels by the node that compiled them (``local`` for passthrough)."""
    groups: dict[str, list[str]] = {}
    for r in records:
        if r.status != "ok":
            continue
        key = r.node or "local"
        groups.setdefault(key, []).append(r.source)
    return sorted(groups.items())

