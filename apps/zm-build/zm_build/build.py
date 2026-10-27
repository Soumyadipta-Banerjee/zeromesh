"""Run CMake or Make with recc pointed at ZeroMesh REAPI."""

from __future__ import annotations

import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, TextIO

from zm_build.env import (
    DEFAULT_REAPI_SERVER,
    apply_make_launchers,
    detect_project,
    recc_env,
    require_tool,
)


@dataclass
class BuildOptions:
    source_dir: Path = Path(".")
    build_dir: Path = Path("build")
    server: str = DEFAULT_REAPI_SERVER
    jobs: int = 0  # 0 = tool default
    targets: list[str] = field(default_factory=list)
    cmake_args: list[str] = field(default_factory=list)
    make_args: list[str] = field(default_factory=list)
    configure_only: bool = False
    build_only: bool = False
    check_daemon: bool = True
    socket_path: Optional[str] = None
    verbose: bool = False


def check_recc() -> str:
    return require_tool(
        "recc",
        hint="install with scripts/install_recc.sh",
    )


def check_daemon(socket_path: Optional[str] = None) -> None:
    try:
        import zeromesh
    except ImportError as e:
        raise RuntimeError(
            "zeromesh SDK not installed (pip install ./sdk/python)"
        ) from e
    try:
        client = zeromesh.connect(socket_path=socket_path)
    except zeromesh.DaemonNotRunning as e:
        raise RuntimeError(
            "zmd is not running — start with: "
            "ZM_ENABLE_REAPI=1 ZM_PLUGIN_PATH=./build ./build/zmd"
        ) from e
    client.close()


def warn_reapi(stream: TextIO = sys.stderr) -> None:
    print(
        "zm-build: ensure zmd loaded frontend-reapi "
        "(ZM_ENABLE_REAPI=1 and ZM_PLUGIN_PATH includes libzm_frontend_reapi.so)",
        file=stream,
    )


def run_cmake(opts: BuildOptions, stream: TextIO = sys.stderr) -> int:
    check_recc()
    require_tool("cmake")
    if opts.check_daemon:
        check_daemon(opts.socket_path)
        warn_reapi(stream)

    src = opts.source_dir.resolve()
    build = opts.build_dir.resolve()
    if not (src / "CMakeLists.txt").is_file() and not opts.build_only:
        raise FileNotFoundError(f"no CMakeLists.txt in {src}")

    env = recc_env(server=opts.server, project_root=src)
    if opts.verbose:
        env["RECC_VERBOSE"] = "1"

    build.mkdir(parents=True, exist_ok=True)

    if not opts.build_only:
        configure = [
            "cmake",
            "-S",
            str(src),
            "-B",
            str(build),
            "-DCMAKE_C_COMPILER_LAUNCHER=recc",
            "-DCMAKE_CXX_COMPILER_LAUNCHER=recc",
            *opts.cmake_args,
        ]
        print("+", " ".join(configure), file=stream)
        res = subprocess.run(configure, env=env)
        if res.returncode != 0:
            return res.returncode

    if opts.configure_only:
        return 0

    build_cmd = ["cmake", "--build", str(build)]
    if opts.jobs > 0:
        build_cmd.extend(["--parallel", str(opts.jobs)])
    if opts.targets:
        for t in opts.targets:
            build_cmd.extend(["--target", t])
    print("+", " ".join(build_cmd), file=stream)
    return subprocess.run(build_cmd, env=env).returncode


def run_make(opts: BuildOptions, stream: TextIO = sys.stderr) -> int:
    check_recc()
    require_tool("make")
    if opts.check_daemon:
        check_daemon(opts.socket_path)
        warn_reapi(stream)

    src = opts.source_dir.resolve()
    env = recc_env(server=opts.server, project_root=src)
    apply_make_launchers(env)
    if opts.verbose:
        env["RECC_VERBOSE"] = "1"

    cmd = ["make", "-C", str(src)]
    if opts.jobs > 0:
        cmd.append(f"-j{opts.jobs}")
    cmd.extend(opts.targets)
    cmd.extend(opts.make_args)
    print("+", " ".join(cmd), file=stream)
    print(f"+ CC={env['CC']} CXX={env['CXX']}", file=stream)
    return subprocess.run(cmd, env=env).returncode


def run_auto(opts: BuildOptions, stream: TextIO = sys.stderr) -> int:
    kind = detect_project(opts.source_dir)
    if kind == "cmake":
        return run_cmake(opts, stream=stream)
    return run_make(opts, stream=stream)


def which_reapi_hint() -> str:
    return (
        "REAPI path: recc → 127.0.0.1:8980 → plugins/frontend-reapi → "
        "zmd ClientApi → team donors"
    )
