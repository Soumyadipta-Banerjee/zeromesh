"""zm-build / zm-cmake / zm-make entry points."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from zm_build.build import BuildOptions, run_auto, run_cmake, run_make
from zm_build.env import DEFAULT_REAPI_SERVER, MissingToolError


def _add_common(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--server",
        default=DEFAULT_REAPI_SERVER,
        help=f"REAPI host:port (default: {DEFAULT_REAPI_SERVER})",
    )
    p.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=0,
        metavar="N",
        help="Parallel jobs (cmake --parallel / make -j)",
    )
    p.add_argument(
        "--socket",
        default=None,
        help="Override zmd unix socket path",
    )
    p.add_argument(
        "--no-daemon-check",
        action="store_true",
        help="Skip the zmd connectivity check",
    )
    p.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Set RECC_VERBOSE=1",
    )


def build_cmake_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="zm-cmake",
        description=(
            "Configure and build a CMake project with recc → ZeroMesh REAPI. "
            "Does not replace CMake — only sets the compiler launcher."
        ),
    )
    p.add_argument("-S", "--source-dir", type=Path, default=Path("."), dest="source_dir")
    p.add_argument("-B", "--build-dir", type=Path, default=Path("build"), dest="build_dir")
    p.add_argument(
        "--target",
        action="append",
        default=[],
        dest="targets",
        metavar="NAME",
        help="Build target (repeatable)",
    )
    p.add_argument("--configure-only", action="store_true")
    p.add_argument("--build-only", action="store_true")
    _add_common(p)
    p.add_argument(
        "cmake_args",
        nargs="*",
        help="Extra args forwarded to cmake configure (after --)",
    )
    return p


def build_make_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="zm-make",
        description=(
            "Run Make with CC/CXX wrapped by recc → ZeroMesh REAPI. "
            "Does not replace Make — only wraps the compilers."
        ),
    )
    p.add_argument("-C", "--directory", type=Path, default=Path("."), dest="source_dir")
    p.add_argument(
        "targets",
        nargs="*",
        help="Make targets and extra make args",
    )
    _add_common(p)
    return p


def build_auto_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="zm-build",
        description=(
            "Auto-detect CMakeLists.txt or Makefile and build through "
            "recc → ZeroMesh REAPI."
        ),
    )
    p.add_argument(
        "path",
        type=Path,
        nargs="?",
        default=Path("."),
        help="Project directory (default: .)",
    )
    p.add_argument("-B", "--build-dir", type=Path, default=Path("build"), dest="build_dir")
    _add_common(p)
    return p


def _opts_from_common(args: argparse.Namespace, source_dir: Path) -> BuildOptions:
    return BuildOptions(
        source_dir=source_dir,
        build_dir=getattr(args, "build_dir", Path("build")),
        server=args.server,
        jobs=args.jobs,
        targets=list(getattr(args, "targets", []) or []),
        cmake_args=list(getattr(args, "cmake_args", []) or []),
        configure_only=bool(getattr(args, "configure_only", False)),
        build_only=bool(getattr(args, "build_only", False)),
        check_daemon=not args.no_daemon_check,
        socket_path=args.socket,
        verbose=args.verbose,
    )


def _run(fn, opts: BuildOptions) -> int:
    try:
        return fn(opts)
    except MissingToolError as e:
        print(f"zm-build: {e}", file=sys.stderr)
        return 1
    except FileNotFoundError as e:
        print(f"zm-build: {e}", file=sys.stderr)
        return 1
    except RuntimeError as e:
        print(f"zm-build: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("zm-build: cancelled", file=sys.stderr)
        return 130


def main_cmake(argv: list[str] | None = None) -> int:
    args = build_cmake_parser().parse_args(argv)
    return _run(run_cmake, _opts_from_common(args, args.source_dir))


def main_make(argv: list[str] | None = None) -> int:
    args = build_make_parser().parse_args(argv)
    return _run(run_make, _opts_from_common(args, args.source_dir))


def main_build(argv: list[str] | None = None) -> int:
    args = build_auto_parser().parse_args(argv)
    return _run(run_auto, _opts_from_common(args, args.path))


if __name__ == "__main__":
    raise SystemExit(main_build())
