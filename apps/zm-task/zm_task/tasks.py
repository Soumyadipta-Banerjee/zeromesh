"""Parse run.sh into distributable task commands."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class TaskLine:
    """A single executable command parsed from run.sh."""

    index: int       # 0-based line index (only counting executable lines)
    command: str     # the shell command string
    label: str       # human-friendly label (e.g. "task_0", or extracted from comment)

    @property
    def name(self) -> str:
        """ZeroMesh task name (must be unique within a job)."""
        return f"task_{self.index}"


def parse_run_script(path: Path) -> list[TaskLine]:
    """Parse a run.sh into a list of executable TaskLines.

    Rules:
        - Blank lines and comment-only lines are skipped.
        - A ``# @label <name>`` comment on the line before a command sets
          the label for that command.
        - Line continuations (trailing ``\\``) are joined.
        - ``set -e``, ``set -x``, etc. are kept as-is (they're valid
          commands the user wants).
    """
    if not path.is_file():
        raise FileNotFoundError(f"run.sh not found: {path}")
    text = path.read_text(encoding="utf-8", errors="replace")
    return parse_run_text(text)


def parse_run_text(text: str) -> list[TaskLine]:
    """Parse run script text content into TaskLines."""
    raw_lines = text.splitlines()

    # Join line continuations
    joined: list[str] = []
    buf = ""
    for ln in raw_lines:
        stripped = ln.rstrip()
        if stripped.endswith("\\"):
            buf += stripped[:-1] + " "
        else:
            buf += stripped
            joined.append(buf)
            buf = ""
    if buf.strip():
        joined.append(buf)

    tasks: list[TaskLine] = []
    pending_label: str | None = None
    idx = 0

    for line in joined:
        s = line.strip()

        # Skip blank
        if not s:
            continue

        # Skip shebang
        if s.startswith("#!"):
            continue

        # Check for @label directive
        m = re.match(r"^#\s*@label\s+(.+)$", s)
        if m:
            pending_label = m.group(1).strip()
            continue

        # Skip pure comments (but NOT set/export which are commands)
        if s.startswith("#"):
            continue

        # This is an executable line
        label = pending_label if pending_label else f"task_{idx}"
        tasks.append(TaskLine(index=idx, command=s, label=label))
        pending_label = None
        idx += 1

    return tasks


def split_task_groups(tasks: list[TaskLine], group_size: int) -> list[list[TaskLine]]:
    """Split tasks into groups of at most group_size for batching.

    Each group runs sequentially on one node; groups run in parallel.
    """
    if group_size < 1:
        group_size = 1
    groups: list[list[TaskLine]] = []
    for i in range(0, len(tasks), group_size):
        groups.append(tasks[i : i + group_size])
    return groups
