"""Partial commit: save each finished unit of a task's work as it completes.

Inside a ZeroMesh task, call :func:`commit_unit` when a frame or a sweep case is done. The
daemon stores the unit as soon as it appears. If the task is interrupted (its donor leaves,
it times out, the job is run again), the next attempt starts with every committed unit
present, so :func:`is_committed` / :func:`unit_result` let it skip the finished work.
"""

import os
import shutil
import tempfile
from typing import List, Optional, Union

from zeromesh.errors import ZeroMeshError

UNITS_ENV = "ZM_UNITS_DIR"
_MAX_ID = 128
_ALLOWED = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")


def units_dir() -> Optional[str]:
    """The directory units are committed to, or None when not running inside a task."""
    return os.environ.get(UNITS_ENV) or None


def _check_id(unit_id: str) -> None:
    if not unit_id or len(unit_id) > _MAX_ID or unit_id[0] == "." or not set(unit_id) <= _ALLOWED:
        raise ValueError(
            f"invalid unit id {unit_id!r}: use 1-{_MAX_ID} characters from A-Z a-z 0-9 . _ -, "
            "not starting with '.'"
        )


def commit_unit(unit_id: str, data: Union[bytes, str, "os.PathLike[str]"]) -> str:
    """Commits `data` as unit `unit_id` and returns the path it was written to.

    `data` is bytes, text (written as UTF-8) or a path to a file, which is copied. The write
    is atomic: the daemon never sees a half-written unit. Committing an id again replaces it.
    """
    _check_id(unit_id)
    base = units_dir()
    if base is None:
        raise ZeroMeshError(f"commit_unit works only inside a ZeroMesh task ({UNITS_ENV} is not set)")
    os.makedirs(base, exist_ok=True)
    final = os.path.join(base, unit_id)
    fd, staging = tempfile.mkstemp(prefix=".tmp-", dir=base)  # dot-name: ignored by the daemon
    try:
        with os.fdopen(fd, "wb") as out:
            if isinstance(data, bytes):
                out.write(data)
            elif isinstance(data, str):
                out.write(data.encode("utf-8"))
            else:
                with open(os.fspath(data), "rb") as src:
                    shutil.copyfileobj(src, out)
        os.replace(staging, final)
    except BaseException:
        try:
            os.unlink(staging)
        except OSError:
            pass
        raise
    return final


def committed_units() -> List[str]:
    """Ids of the units committed so far (including those an earlier attempt committed)."""
    base = units_dir()
    if base is None or not os.path.isdir(base):
        return []
    return sorted(n for n in os.listdir(base) if not n.startswith(".") and os.path.isfile(os.path.join(base, n)))


def is_committed(unit_id: str) -> bool:
    return unit_id in committed_units()


def unit_result(unit_id: str) -> Optional[bytes]:
    """The bytes of a committed unit, or None if there is no such unit."""
    base = units_dir()
    if base is None:
        return None
    try:
        _check_id(unit_id)
        with open(os.path.join(base, unit_id), "rb") as f:
            return f.read()
    except (OSError, ValueError):
        return None
