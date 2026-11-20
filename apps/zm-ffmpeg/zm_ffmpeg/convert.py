"""Input validation and deterministic conversion plans for zm-ffmpeg."""

from __future__ import annotations

import hashlib
import os
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

_SUFFIX = re.compile(r"^\.[A-Za-z0-9][A-Za-z0-9_-]*$")
_SLUG = re.compile(r"[^A-Za-z0-9_.-]+")


@dataclass(frozen=True)
class Conversion:
    input: Path
    output: Path
    id: str


def _relative_file(raw: Path, what: str) -> Path:
    if raw.is_absolute():
        raise ValueError(f"{what} must be relative to the workspace: {raw}")
    normalized = Path(os.path.normpath(str(raw)))
    if str(normalized) in ("", ".", "..") or str(normalized).startswith("../"):
        raise ValueError(f"{what} must stay inside the workspace: {raw}")
    return normalized


def _item_id(input_path: Path) -> str:
    digest = hashlib.sha256(input_path.as_posix().encode("utf-8")).hexdigest()
    slug = _SLUG.sub("_", input_path.stem)[:40].strip("._-") or "convert"
    return f"{slug}-{digest}"


def plan_conversions(inputs: Sequence[Path], suffix: str) -> list[Conversion]:
    """One deterministic output per input; refuse ambiguous or unsafe plans before connecting."""
    if not _SUFFIX.fullmatch(suffix):
        raise ValueError("suffix must look like .mp4, .mkv, .webm, or .mp3 (letters, digits, '_' or '-')")

    conversions: dict[Path, Conversion] = {}
    seen_inputs: set[Path] = set()
    for raw in inputs:
        source = _relative_file(raw, "input")
        if source in seen_inputs:
            raise ValueError(f"duplicate input: {source}")
        seen_inputs.add(source)
        output = _relative_file(source.with_suffix(suffix), "output")
        if output == source:
            raise ValueError(f"input and output are the same path: {source}; choose a different suffix")
        if output in conversions:
            raise ValueError(f"output collision: {conversions[output].input} and {source} both produce {output}")
        conversions[output] = Conversion(source, output, _item_id(source))
    return sorted(conversions.values(), key=lambda conversion: conversion.input.as_posix())


def check_workspace_file(workspace: Path, conversion: Conversion) -> None:
    """Require a regular, workspace-contained input; external symlinks are not implicit uploads."""
    root = workspace.resolve()
    path = root / conversion.input
    try:
        path.resolve().relative_to(root)
    except ValueError:
        raise ValueError(f"input resolves outside the workspace: {conversion.input}") from None
    if not path.is_file():
        raise ValueError(f"input is not a regular file: {conversion.input}")


def ffmpeg_argv(ffmpeg: str, conversion: Conversion, options: Sequence[str]) -> list[str]:
    """Construct argv directly: no shell quoting or evaluation of user-supplied options."""
    return [ffmpeg, "-nostdin", "-y", "-i", conversion.input.as_posix(), *options, conversion.output.as_posix()]
