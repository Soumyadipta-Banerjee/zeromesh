"""RECC / REAPI environment helpers."""

from __future__ import annotations

import os
import shutil
from pathlib import Path
from typing import Mapping, MutableMapping, Optional


DEFAULT_REAPI_SERVER = "127.0.0.1:8980"


class MissingToolError(RuntimeError):
    """A required local binary is not on PATH."""

    def __init__(self, tool: str, hint: str = ""):
        self.tool = tool
        msg = f"{tool} not found on PATH"
        if hint:
            msg = f"{msg} ({hint})"
        super().__init__(msg)


def require_tool(name: str, hint: str = "") -> str:
    path = shutil.which(name)
    if path is None:
        raise MissingToolError(name, hint)
    return path


def recc_env(
    server: str = DEFAULT_REAPI_SERVER,
    project_root: Optional[Path] = None,
    extra: Optional[Mapping[str, str]] = None,
    base: Optional[Mapping[str, str]] = None,
) -> dict[str, str]:
    """Environment for recc-backed CMake/Make builds.

    Isolates from a caller's RECC_LINK so CMake's normal final link stays local
    (same as integration_reapi).
    """
    env: dict[str, str] = dict(base if base is not None else os.environ)
    env.pop("RECC_LINK", None)
    root = str((project_root or Path.cwd()).resolve())
    env.update(
        {
            "RECC_SERVER": server,
            "RECC_CAS_SERVER": server,
            "RECC_INSTANCE": "default",
            "RECC_ACTION_CACHE_SERVERS": server,
            "RECC_PROJECT_ROOT": root,
            "RECC_DEPS_GLOBAL_PATHS": "1",
            "CMAKE_C_COMPILER_LAUNCHER": "recc",
            "CMAKE_CXX_COMPILER_LAUNCHER": "recc",
        }
    )
    if extra:
        env.update(extra)
    return env


def apply_make_launchers(env: MutableMapping[str, str]) -> None:
    """Point Make's CC/CXX through recc without replacing the real compilers."""
    cc = env.get("CC") or "cc"
    cxx = env.get("CXX") or "c++"
    # Avoid double-wrapping if the user already set CC='recc cc'.
    if not cc.strip().startswith("recc"):
        env["CC"] = f"recc {cc}"
    if not cxx.strip().startswith("recc"):
        env["CXX"] = f"recc {cxx}"


def detect_project(path: Path) -> str:
    """Return 'cmake', 'make', or raise if neither is present."""
    path = path.resolve()
    if (path / "CMakeLists.txt").is_file():
        return "cmake"
    if (path / "Makefile").is_file() or (path / "makefile").is_file():
        return "make"
    raise FileNotFoundError(
        f"no CMakeLists.txt or Makefile under {path} "
        "(pass -S/-C or use zm-cmake / zm-make explicitly)"
    )
