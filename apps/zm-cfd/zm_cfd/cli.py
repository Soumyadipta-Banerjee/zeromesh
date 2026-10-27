"""Command-line entry point for OpenFOAM case sweeps."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from zm_cfd.runner import CfdOptions, run_cfd


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="zm-cfd",
        description="Run independent OpenFOAM cases across a ZeroMesh team.",
    )
    parser.add_argument("cases", type=Path, help="one OpenFOAM case or a directory of case directories")
    parser.add_argument("--solver", default="simpleFoam", help="solver executable (default: simpleFoam)")
    parser.add_argument("--solver-arg", action="append", default=[], help="solver argument; repeat as needed")
    parser.add_argument(
        "--bashrc", type=Path,
        help="OpenFOAM environment script to source on the worker before running the solver",
    )
    parser.add_argument("--env", choices=("host",), default="host")
    parser.add_argument("--cores", type=int, default=1, help="cores requested per case task")
    parser.add_argument("-o", "--output", type=Path, default=Path("cfd-results"))
    placement = parser.add_mutually_exclusive_group()
    placement.add_argument("--remote-only", action="store_true", help="run cases only on peer nodes")
    placement.add_argument("--local-only", action="store_true", help="run cases only on this machine")
    parser.add_argument("--socket", help="override local zmd socket path")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.cores < 1:
        print("zm-cfd: --cores must be >= 1", file=sys.stderr)
        return 2
    try:
        import zeromesh
    except ImportError:
        print("zm-cfd: ZeroMesh SDK missing (pip install ./sdk/python)", file=sys.stderr)
        return 1

    try:
        client = zeromesh.connect(socket_path=args.socket)
        options = CfdOptions(
            cases=args.cases,
            solver=args.solver,
            solver_args=args.solver_arg,
            bashrc=args.bashrc,
            env=args.env,
            cores=args.cores,
            out_dir=args.output,
            may_run_local=not args.remote_only,
            may_run_remote=not args.local_only,
        )
        with client:
            result = run_cfd(client, options)
    except zeromesh.DaemonNotRunning:
        print("zmd is not running (start it with: zmd)", file=sys.stderr)
        return 1
    except (FileNotFoundError, ValueError) as error:
        print(f"zm-cfd: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("zm-cfd: cancelled", file=sys.stderr)
        return 130

    if result.failed:
        names = ", ".join(item.case.name for item in result.failed)
        print(f"zm-cfd: failed cases: {names}", file=sys.stderr)
        return 1
    print(f"zm-cfd: collected {len(result.cases)} case results in {options.out_dir}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
