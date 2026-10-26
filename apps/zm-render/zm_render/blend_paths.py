"""Detect absolute paths referenced by a .blend that sit outside its directory."""

from __future__ import annotations

import os
import re
from pathlib import Path

# Null-terminated-ish absolute Unix paths that look like file references in .blend DNA.
_ABS_PATH_RE = re.compile(rb"(/(?:home|usr|opt|var|tmp|mnt|media)/[^\x00\r\n]{2,500})")


def find_external_absolute_paths(blend_path: str | os.PathLike) -> list[str]:
    """Return absolute path strings found in the .blend that fall outside its parent dir.

    This is a best-effort binary scan (no Blender required). Mirror mode can still
    resolve host paths; host mode donors will not have them.
    """
    path = Path(blend_path).resolve()
    root = path.parent
    try:
        data = path.read_bytes()
    except OSError:
        return []

    found: list[str] = []
    seen: set[str] = set()
    for m in _ABS_PATH_RE.finditer(data):
        raw = m.group(0)
        # Trim at first non-printable / unlikely path char
        cleaned = raw.split(b"\x00", 1)[0]
        try:
            text = cleaned.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            continue
        # Drop trailing junk that often trails DNA strings
        text = text.rstrip(" \t")
        while text and text[-1] in "\"')]>.,;":
            text = text[:-1]
        if len(text) < 2 or not text.startswith("/"):
            continue
        if text in seen:
            continue
        try:
            candidate = Path(text)
            # Only warn when the path is outside the scene directory.
            if root in candidate.parents or candidate == root or candidate == path:
                continue
            # Also skip the blend file itself if stored absolutely.
            if candidate.resolve() == path:
                continue
        except (OSError, RuntimeError, ValueError):
            pass
        seen.add(text)
        found.append(text)
    return found


def warn_external_paths(blend_path: str | os.PathLike, file=None) -> list[str]:
    """Print a warning for external absolute paths; return the list found."""
    paths = find_external_absolute_paths(blend_path)
    if not paths:
        return []
    out = file
    print(
        "warning: .blend references absolute paths outside the scene directory;\n"
        "  donors will not see them unless --env mirror is used.\n"
        "  Prefer File → External Data → Make Paths Relative, or pack resources.",
        file=out,
    )
    for p in paths[:20]:
        print(f"  - {p}", file=out)
    if len(paths) > 20:
        print(f"  … and {len(paths) - 20} more", file=out)
    return paths
